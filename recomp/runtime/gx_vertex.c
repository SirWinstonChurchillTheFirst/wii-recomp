// Vertices: decodes each format the game declares into a buffer for the vertex
// shader, which applies the matrices (gx_transform.c) and lights (gx_light.c).
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "debug.h"
#include "gx.h"
#include "memory_watch.h"

enum {
  CP_MATRIX_INDEX = 0x30, CP_DESCRIPTOR_LOW = 0x50, CP_DESCRIPTOR_HIGH = 0x60, CP_FORMAT_A = 0x70,
  CP_FORMAT_B = 0x80, CP_FORMAT_C = 0x90, CP_ARRAY_BASE = 0xa0, CP_ARRAY_STRIDE = 0xb0,
  ARRAY_POSITION = 0, ARRAY_NORMAL = 1, ARRAY_COLOR = 2, ARRAY_TEXTURE = 4,
  KIND_NONE = 0, KIND_DIRECT = 1, KIND_INDEX8 = 2, KIND_INDEX16 = 3,
  COMPONENT_U8 = 0, COMPONENT_S8 = 1, COMPONENT_U16 = 2, COMPONENT_S16 = 3, COMPONENT_FLOAT = 4,
  COLOR_RGB565 = 0, COLOR_RGB8 = 1, COLOR_RGBX8 = 2, COLOR_RGBA4 = 3, COLOR_RGBA6 = 4, COLOR_RGBA8 = 5,
  NORMAL_SHIFT_U8 = 7, NORMAL_SHIFT_S8 = 6, NORMAL_SHIFT_U16 = 15, NORMAL_SHIFT_S16 = 14, MATRIX_INDEX_MASK = 63, TEXTURES = 8, ALL_NORMAL_VALUES = 9,
  NULL_INDEX8 = 0xff, NULL_INDEX16 = 0xffff, DEQUANTIZE_BIT = 30,
};

float gx_cached_normals[CACHED_NORMALS][4];

typedef struct {
  int position, normal, color[2], texture[TEXTURES], position_index, texture_index[TEXTURES];
  int position_components, position_type, position_shift;
  int normal_components, normal_type, normal_indexes;
  int color_type[2], color_alpha[2];
  int texture_components[TEXTURES], texture_type[TEXTURES], texture_shift[TEXTURES];
  float position_scale, normal_scale, texture_scale[TEXTURES];  // From the shifts, worked out once a draw.
} Format;

static uint32_t bits(uint32_t value, int low, int count) { return (value >> low) & ((1u << count) - 1); }

static int normal_shift(int type) {
  return type == COMPONENT_U8 ? NORMAL_SHIFT_U8 : type == COMPONENT_S8 ? NORMAL_SHIFT_S8 : type == COMPONENT_U16 ? NORMAL_SHIFT_U16 : NORMAL_SHIFT_S16;
}

static void read_format(int v, Format *f) {
  uint32_t low = gx.cp[CP_DESCRIPTOR_LOW], high = gx.cp[CP_DESCRIPTOR_HIGH];
  uint32_t a = gx.cp[CP_FORMAT_A + v], b = gx.cp[CP_FORMAT_B + v], c = gx.cp[CP_FORMAT_C + v];
  f->position_index = (int)(low & 1);
  for (int i = 0; i < TEXTURES; i++) {
    f->texture_index[i] = (int)bits(low, 1 + i, 1);
    f->texture[i] = (int)bits(high, i * 2, 2);
  }
  f->position = (int)bits(low, 9, 2);
  f->normal = (int)bits(low, 11, 2);
  f->color[0] = (int)bits(low, 13, 2);
  f->color[1] = (int)bits(low, 15, 2);
  f->position_components = bits(a, 0, 1) ? 3 : 2;
  f->position_type = (int)bits(a, 1, 3);
  f->position_shift = (int)bits(a, 4, 5);
  f->normal_components = bits(a, 9, 1) ? 9 : 3;
  f->normal_type = (int)bits(a, 10, 3);
  // Three normals can each have their own index, one after the other.
  f->normal_indexes = bits(a, 31, 1) && f->normal_components == ALL_NORMAL_VALUES && f->normal >= KIND_INDEX8 ? 3 : 1;
  f->color_alpha[0] = (int)bits(a, 13, 1);
  f->color_type[0] = (int)bits(a, 14, 3);
  f->color_alpha[1] = (int)bits(a, 17, 1);
  f->color_type[1] = (int)bits(a, 18, 3);
  static const struct { int word, count, type, shift; } layout[TEXTURES] = {
    {0, 21, 22, 25}, {1, 0, 1, 4}, {1, 9, 10, 13}, {1, 18, 19, 22},
    {1, 27, 28, -1}, {2, 5, 6, 9}, {2, 14, 15, 18}, {2, 23, 24, 27},
  };
  const uint32_t words[3] = {a, b, c};
  for (int i = 0; i < TEXTURES; i++) {
    uint32_t word = words[layout[i].word];
    f->texture_components[i] = bits(word, layout[i].count, 1) ? 2 : 1;
    f->texture_type[i] = (int)bits(word, layout[i].type, 3);
    f->texture_shift[i] = layout[i].shift < 0 ? (int)bits(c, 0, 5) : (int)bits(word, layout[i].shift, 5);
  }
  // With the dequantize bit clear, whole-number positions and coordinates keep their value
  // whatever their shift, as Dolphin's compiled vertex loaders read it. The SDK always sets it.
  if (!bits(a, DEQUANTIZE_BIT, 1)) {
    f->position_shift = 0;
    for (int i = 0; i < TEXTURES; i++) f->texture_shift[i] = 0;
  }
  f->position_scale = 1.0f / (float)(1u << f->position_shift);
  for (int i = 0; i < TEXTURES; i++) f->texture_scale[i] = 1.0f / (float)(1u << f->texture_shift[i]);
  f->normal_scale = 1.0f / (float)(1u << normal_shift(f->normal_type));
}

static uint32_t vertex_bytes(const Format *f);

// Each format as last worked out, until the game writes a vertex register again. Every
// draw asks three times: for its size, its normals and its vertices.
static Format formats[8];
static uint32_t format_bytes[8];
static unsigned format_written[8];

static const Format *format_for(int v) {
  if (format_written[v] != gx_cp_written) {
    read_format(v, &formats[v]);
    format_bytes[v] = vertex_bytes(&formats[v]);
    format_written[v] = gx_cp_written;
  }
  return &formats[v];
}

static uint32_t component_bytes(int type) {
  return type == COMPONENT_FLOAT ? 4 : type >= COMPONENT_U16 ? 2 : 1;
}

static uint32_t color_bytes(int type) {
  static const uint32_t sizes[] = {2, 3, 4, 2, 3, 4};
  return sizes[type > COLOR_RGBA8 ? 0 : type];
}

static uint32_t attribute_bytes(int kind, uint32_t direct) {
  if (kind == KIND_DIRECT) return direct;
  return kind == KIND_INDEX8 ? 1 : kind == KIND_INDEX16 ? 2 : 0;
}

static uint32_t vertex_bytes(const Format *f) {
  uint32_t size = (uint32_t)f->position_index;
  for (int i = 0; i < TEXTURES; i++) size += (uint32_t)f->texture_index[i];
  size += attribute_bytes(f->position, (uint32_t)f->position_components * component_bytes(f->position_type));
  size += attribute_bytes(f->normal, (uint32_t)f->normal_components * component_bytes(f->normal_type)) * (uint32_t)f->normal_indexes;
  for (int i = 0; i < 2; i++) size += attribute_bytes(f->color[i], color_bytes(f->color_type[i]));
  for (int i = 0; i < TEXTURES; i++) {
    size += attribute_bytes(f->texture[i], (uint32_t)f->texture_components[i] * component_bytes(f->texture_type[i]));
  }
  return size;
}

int gx_vertex_normals(int format) {
  const Format *f = format_for(format & 7);
  return f->normal == KIND_NONE ? 0 : f->normal_components == ALL_NORMAL_VALUES ? 3 : 1;
}

uint32_t gx_vertices_size(int format, int count) {
  format_for(format & 7);
  return format_bytes[format & 7] * (uint32_t)count;
}

static inline float read_component(const uint8_t *p, int type, float scale) {
  switch (type) {
    case COMPONENT_U8: return (float)p[0] * scale;
    case COMPONENT_S8: return (float)(int8_t)p[0] * scale;
    case COMPONENT_U16: return (float)(uint16_t)((p[0] << 8) | p[1]) * scale;
    case COMPONENT_S16: return (float)(int16_t)((p[0] << 8) | p[1]) * scale;
    default: return as_float(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
  }
}

static void read_color(const uint8_t *p, int type, float out[4]) {
  uint32_t v = 0;
  switch (type) {
    case COLOR_RGB565:
      v = ((uint32_t)p[0] << 8) | p[1];
      out[0] = (float)(v >> 11) / 31; out[1] = (float)((v >> 5) & 63) / 63; out[2] = (float)(v & 31) / 31; out[3] = 1;
      break;
    case COLOR_RGB8: case COLOR_RGBX8:
      out[0] = (float)p[0] / 255; out[1] = (float)p[1] / 255; out[2] = (float)p[2] / 255; out[3] = 1;
      break;
    case COLOR_RGBA4:
      out[0] = (float)(p[0] >> 4) / 15; out[1] = (float)(p[0] & 15) / 15;
      out[2] = (float)(p[1] >> 4) / 15; out[3] = (float)(p[1] & 15) / 15;
      break;
    case COLOR_RGBA6:
      v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
      for (int i = 0; i < 4; i++) out[i] = (float)((v >> (18 - i * 6)) & 63) / 63;
      break;
    default:
      for (int i = 0; i < 4; i++) out[i] = (float)p[i] / 255;
  }
}

enum { ARRAYS = 12 };
// While a display list's draw is decoded to be kept, the lowest and highest index read
// from each array, so the memory it came from can be watched.
static struct { int on; uint32_t low[ARRAYS], high[ARRAYS], used; } spans;
// Each array's base and stride, read once a draw: nothing writes them while it decodes.
static uint32_t array_base[ARRAYS], array_stride[ARRAYS];

// Returns where an attribute's data is, in the stream or in an array.
static const uint8_t *locate(const uint8_t **p, int kind, int array, uint32_t direct) {
  const uint8_t *at = *p;
  if (kind == KIND_DIRECT) {
    *p += direct;
    return at;
  }
  uint32_t index = kind == KIND_INDEX8 ? at[0] : ((uint32_t)at[0] << 8) | at[1];
  *p += kind == KIND_INDEX8 ? 1 : 2;
  if (spans.on) {
    if (!(spans.used & (1u << array)) || index < spans.low[array]) spans.low[array] = index;
    if (!(spans.used & (1u << array)) || index > spans.high[array]) spans.high[array] = index;
    spans.used |= 1u << array;
  }
  return host((array_base[array] + index * array_stride[array]) | 0x80000000u);
}

// Several components of one type, the type chosen once rather than for each.
static inline void read_components(const uint8_t *p, int type, float scale, int count, float *out) {
  switch (type) {
    case COMPONENT_U8: for (int i = 0; i < count; i++) out[i] = (float)p[i] * scale; break;
    case COMPONENT_S8: for (int i = 0; i < count; i++) out[i] = (float)(int8_t)p[i] * scale; break;
    case COMPONENT_U16: for (int i = 0; i < count; i++) out[i] = (float)(uint16_t)((p[i * 2] << 8) | p[i * 2 + 1]) * scale; break;
    case COMPONENT_S16: for (int i = 0; i < count; i++) out[i] = (float)(int16_t)((p[i * 2] << 8) | p[i * 2 + 1]) * scale; break;
    default:
      for (int i = 0; i < count; i++) out[i] = read_component(p + i * 4, COMPONENT_FLOAT, scale);
  }
}

static void read_normal(const uint8_t *source, const Format *f, float out[3]) {
  read_components(source, f->normal_type, f->normal_scale, 3, out);
}

static void read_normals(const uint8_t **p, const Format *f, Vertex *v) {
  uint32_t size = component_bytes(f->normal_type), values = (uint32_t)f->normal_components;
  float *parts[3] = {v->normal, v->tangent, v->binormal};
  if (f->normal_indexes == 1) {
    const uint8_t *source = locate(p, f->normal, ARRAY_NORMAL, values * size);
    for (uint32_t part = 0; source && part < values / 3; part++) read_normal(source + part * 3 * size, f, parts[part]);
    return;
  }
  for (uint32_t part = 0; part < 3; part++) {
    const uint8_t *source = locate(p, f->normal, ARRAY_NORMAL, 0);
    if (source) read_normal(source + part * 3 * size, f, parts[part]);
  }
}

// An index of all ones in the position leaves the vertex out, and those after move up.
static int skips_vertex(const uint8_t *p, int kind) {
  if (kind == KIND_INDEX8) return p[0] == NULL_INDEX8;
  return kind == KIND_INDEX16 && (uint32_t)((p[0] << 8) | p[1]) == NULL_INDEX16;
}

// The chip keeps each normal of the last vertex of a draw, for the vertices of later
// draws with none. That counts even if the vertex was left out.
static void keep_last_normals(const Format *f, const Vertex *last) {
  if (f->normal == KIND_NONE) return;
  memcpy(gx_cached_normals[CACHED_NORMAL], last->normal, sizeof last->normal);
  if (f->normal_components != ALL_NORMAL_VALUES) return;
  memcpy(gx_cached_normals[CACHED_TANGENT], last->tangent, sizeof last->tangent);
  memcpy(gx_cached_normals[CACHED_BINORMAL], last->binormal, sizeof last->binormal);
}

// What every vertex starts as: no position, normal or coordinates, white, and the draw's
// matrices. Copied whole for each, which costs less than setting each part.
static void vertex_start(const Format *f, Vertex *start) {
  (void)f;
  uint32_t matrices = gx.cp[CP_MATRIX_INDEX], matrices_high = gx.cp[CP_MATRIX_INDEX + 0x10];
  memset(start, 0, sizeof *start);
  start->matrix[VERTEX_MATRIX_POSITION] = (uint8_t)(matrices & MATRIX_INDEX_MASK);
  for (int i = 0; i < TEXTURES; i++) {
    uint32_t index = i < 4 ? bits(matrices, 6 + i * 6, 6) : bits(matrices_high, (i - 4) * 6, 6);
    start->matrix[VERTEX_MATRIX_COORDINATE + i] = (uint8_t)index;
  }
  for (int i = 0; i < 2; i++) {
    for (int c = 0; c < 4; c++) start->color[i][c] = 1;
  }
}

static int read_vertices_general(const uint8_t *data, int format, int count, Vertex *out) {
  const Format f = *format_for(format & 7);
  if (f.normal_components == ALL_NORMAL_VALUES && f.normal != KIND_NONE && (f.texture[VERTEX_NORMALS_UV] || f.texture[VERTEX_NORMALS_UV + 1] || f.texture[VERTEX_NORMALS_UV + 2])) {
    debug_unsupported("vtx.normal_nbt_9_values", "texture coordinates 5 to 7 beside binormal and tangent", (uint32_t)format);
  }
  // Color 1 without color 0 feeds the first channel, and the second reads as missing, as
  // Dolphin's VertexShaderGen.cpp has it.
  int color_slot[2] = {0, f.color[0] == KIND_NONE ? 0 : 1};
  Vertex start;
  vertex_start(&f, &start);
  uint32_t position_bytes = (uint32_t)f.position_components * component_bytes(f.position_type);
  uint32_t texture_bytes[TEXTURES];
  for (int i = 0; i < TEXTURES; i++) texture_bytes[i] = (uint32_t)f.texture_components[i] * component_bytes(f.texture_type[i]);
  for (int a = 0; a < ARRAYS; a++) array_base[a] = gx.cp[CP_ARRAY_BASE + a], array_stride[a] = gx.cp[CP_ARRAY_STRIDE + a] & 0xff;
  // The parts the format has, in the order they come: a vertex then looks only at those.
  int coordinate_matrices[TEXTURES], coordinate_matrix_count = 0, coordinates[TEXTURES], coordinate_count = 0, colors[2], color_count = 0;
  for (int i = 0; i < TEXTURES; i++) {
    if (f.texture_index[i]) coordinate_matrices[coordinate_matrix_count++] = i;
    if (f.texture[i] != KIND_NONE) coordinates[coordinate_count++] = i;
  }
  for (int i = 0; i < 2; i++) {
    if (f.color[i] != KIND_NONE) colors[color_count++] = i;
  }
  const uint8_t *p = data;
  int kept = 0;
  for (int n = 0; n < count; n++) {
    Vertex *v = &out[kept];
    *v = start;
    if (f.position_index) v->matrix[VERTEX_MATRIX_POSITION] = *p++ & MATRIX_INDEX_MASK;
    for (int k = 0; k < coordinate_matrix_count; k++) v->matrix[VERTEX_MATRIX_COORDINATE + coordinate_matrices[k]] = *p++ & MATRIX_INDEX_MASK;
    int skipped = skips_vertex(p, f.position);
    if (f.position != KIND_NONE) {
      const uint8_t *source = locate(&p, f.position, ARRAY_POSITION, position_bytes);
      if (source) read_components(source, f.position_type, f.position_scale, f.position_components, v->position);
    }
    if (f.normal != KIND_NONE) read_normals(&p, &f, v);
    for (int k = 0; k < color_count; k++) {
      int i = colors[k];
      const uint8_t *source = locate(&p, f.color[i], ARRAY_COLOR + i, color_bytes(f.color_type[i]));
      if (source) read_color(source, f.color_type[i], v->color[color_slot[i]]);
    }
    for (int k = 0; k < coordinate_count; k++) {
      int i = coordinates[k];
      const uint8_t *source = locate(&p, f.texture[i], ARRAY_TEXTURE + i, texture_bytes[i]);
      if (source) read_components(source, f.texture_type[i], f.texture_scale[i], f.texture_components[i], v->uv[i]);
    }
    if (n == count - 1) keep_last_normals(&f, v);
    if (!skipped) kept++;
  }
  return kept;
}

// The same, worked out once a format: each part the vertex has becomes a step, at a fixed
// place in the vertex's bytes, read by a function made for its encoding. Galaxy 2 sends
// 150,000 vertices a frame, nearly all four parts indexed by 16 bits, and the general
// decoder spent about 130 cycles on each: checking each address against the memory map,
// and reloading the format's fields that didn't fit in registers. Mario & Sonic at the
// Olympic Winter Games sends as many in 21,000 draws of some sixty formats, so what a
// draw needs before its first vertex is kept as well, until a vertex register is written.
// Every reader writes what the general decoder would, from the same conversions: colors
// come from tables of those conversions' results, as a division for each value cost more
// than the rest of a vertex.
typedef void (*ReadPart)(const uint8_t *source, float *to, float scale);

static inline uint32_t big16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t big32_at(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

#define READER(name, count, value) \
  static void name(const uint8_t *p, float *to, float scale) { (void)scale; for (int i = 0; i < count; i++) to[i] = value; }
#define READERS(count) \
  READER(read_u8_##count, count, (float)p[i] * scale) \
  READER(read_s8_##count, count, (float)(int8_t)p[i] * scale) \
  READER(read_u16_##count, count, (float)(uint16_t)big16(p + i * 2) * scale) \
  READER(read_s16_##count, count, (float)(int16_t)big16(p + i * 2) * scale) \
  READER(read_float_##count, count, as_float(big32_at(p + i * 4)))
READERS(1) READERS(2) READERS(3)
#undef READERS
#undef READER

static ReadPart components_reader(int type, int count) {
  static const ReadPart readers[3][5] = {
    {read_u8_1, read_s8_1, read_u16_1, read_s16_1, read_float_1},
    {read_u8_2, read_s8_2, read_u16_2, read_s16_2, read_float_2},
    {read_u8_3, read_s8_3, read_u16_3, read_s16_3, read_float_3},
  };
  return count >= 1 && count <= 3 ? readers[count - 1][type > COMPONENT_FLOAT ? COMPONENT_FLOAT : type] : 0;
}

// Each value a color's bits can have, as read_color works it out.
static float of_31[32], of_63[64], of_15[16], of_255[256];
__attribute__((constructor)) static void fill_color_tables(void) {
  for (int i = 0; i < 32; i++) of_31[i] = (float)i / 31;
  for (int i = 0; i < 64; i++) of_63[i] = (float)i / 63;
  for (int i = 0; i < 16; i++) of_15[i] = (float)i / 15;
  for (int i = 0; i < 256; i++) of_255[i] = (float)i / 255;
}

static void read_rgb565(const uint8_t *p, float *to, float scale) {
  (void)scale;
  uint32_t v = big16(p);
  to[0] = of_31[v >> 11], to[1] = of_63[(v >> 5) & 63], to[2] = of_31[v & 31], to[3] = 1;
}
static void read_rgb8(const uint8_t *p, float *to, float scale) {
  (void)scale;
  to[0] = of_255[p[0]], to[1] = of_255[p[1]], to[2] = of_255[p[2]], to[3] = 1;
}
static void read_rgba4(const uint8_t *p, float *to, float scale) {
  (void)scale;
  to[0] = of_15[p[0] >> 4], to[1] = of_15[p[0] & 15], to[2] = of_15[p[1] >> 4], to[3] = of_15[p[1] & 15];
}
static void read_rgba6(const uint8_t *p, float *to, float scale) {
  (void)scale;
  uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
  to[0] = of_63[v >> 18], to[1] = of_63[(v >> 12) & 63], to[2] = of_63[(v >> 6) & 63], to[3] = of_63[v & 63];
}
static void read_rgba8(const uint8_t *p, float *to, float scale) {
  (void)scale;
  to[0] = of_255[p[0]], to[1] = of_255[p[1]], to[2] = of_255[p[2]], to[3] = of_255[p[3]];
}

// A type past the last reads as the last, as read_color has it.
static ReadPart color_reader(int type) {
  static const ReadPart readers[] = {read_rgb565, read_rgb8, read_rgb8, read_rgba4, read_rgba6, read_rgba8};
  return readers[type > COLOR_RGBA8 ? COLOR_RGBA8 : type];
}

enum { MOST_STEPS = 12 };
typedef struct {
  ReadPart read;     // None for a normal with its tangent and binormal, which the vertex keeps apart.
  uint8_t kind, array;
  uint16_t at, out;  // The part's place in the stream's vertex, and in Vertex.
  float scale;
} Step;
typedef struct {
  int ready, steps_count;
  uint32_t bytes;
  Step steps[MOST_STEPS];
} Plan;
static Plan plans[8];
static unsigned plan_written[8];

static void plan_step(Plan *plan, ReadPart read, int kind, int array, uint32_t at, size_t out, float scale) {
  plan->steps[plan->steps_count++] = (Step){read, (uint8_t)kind, (uint8_t)array, (uint16_t)at, (uint16_t)out, scale};
}

static const Plan *plan_for(int format) {
  const Format *f = format_for(format);
  Plan *plan = &plans[format];
  if (plan_written[format] == gx_cp_written) return plan;
  plan_written[format] = gx_cp_written;
  memset(plan, 0, sizeof *plan);
  // Three separately indexed normals keep the general decoder; so do formats too long to plan.
  plan->ready = f->normal_indexes == 1;
  uint32_t at = (uint32_t)f->position_index;
  for (int i = 0; i < TEXTURES; i++) at += (uint32_t)f->texture_index[i];
  if (f->position != KIND_NONE) {
    uint32_t direct = (uint32_t)f->position_components * component_bytes(f->position_type);
    plan_step(plan, components_reader(f->position_type, f->position_components), f->position, ARRAY_POSITION, at, offsetof(Vertex, position), f->position_scale);
    at += attribute_bytes(f->position, direct);
  }
  if (f->normal != KIND_NONE) {
    uint32_t direct = (uint32_t)f->normal_components * component_bytes(f->normal_type);
    plan_step(plan, f->normal_components == 3 ? components_reader(f->normal_type, 3) : 0, f->normal, ARRAY_NORMAL, at, offsetof(Vertex, normal), f->normal_scale);
    at += attribute_bytes(f->normal, direct) * (uint32_t)f->normal_indexes;
  }
  int slot[2] = {0, f->color[0] == KIND_NONE ? 0 : 1};
  for (int i = 0; i < 2; i++) {
    if (f->color[i] == KIND_NONE) continue;
    plan_step(plan, color_reader(f->color_type[i]), f->color[i], ARRAY_COLOR + i, at, offsetof(Vertex, color) + (size_t)slot[i] * sizeof(float[4]), 0);
    at += attribute_bytes(f->color[i], color_bytes(f->color_type[i]));
  }
  for (int i = 0; i < TEXTURES; i++) {
    if (f->texture[i] == KIND_NONE) continue;
    if (plan->steps_count == MOST_STEPS) {
      plan->ready = 0;
      break;
    }
    uint32_t direct = (uint32_t)f->texture_components[i] * component_bytes(f->texture_type[i]);
    plan_step(plan, components_reader(f->texture_type[i], f->texture_components[i]), f->texture[i], ARRAY_TEXTURE + i, at,
              offsetof(Vertex, uv) + (size_t)i * sizeof(float[2]), f->texture_scale[i]);
    at += attribute_bytes(f->texture[i], direct);
  }
  plan->bytes = format_bytes[format];
  return plan;
}

// Where an array's elements start in this process, and how far past its start the
// memory it's in goes: an index whose element starts beyond that has no memory, as host()
// says. No index times stride reaches from one block of memory into another.
typedef struct { const uint8_t *first; uint32_t room, stride; int unmapped; } Array;

static Array array_at(uint32_t base, uint32_t stride) {
  uint32_t physical = base & 0x3fffffffu, room = 0;
  if (physical < MEM1_SIZE) room = MEM1_SIZE - physical;
  else if (physical - MEM2_START < MEM2_SIZE) room = MEM2_START + MEM2_SIZE - physical;
  else if (physical - LOCKED_CACHE_PHYSICAL < LOCKED_CACHE_SIZE) room = LOCKED_CACHE_PHYSICAL + LOCKED_CACHE_SIZE - physical;
  // An array starting where there's no memory could still reach some: those go through host().
  return (Array){GUEST_BASE + physical, room, stride, room == 0};
}

static inline const uint8_t *element(const Array *a, int array, uint32_t index) {
  if (a->unmapped) return host((array_base[array] + index * array_stride[array]) | 0x80000000u);
  uint32_t offset = index * a->stride;
  return offset < a->room ? a->first + offset : 0;
}

// What a draw needs before its first vertex: the vertex each starts as, where each step's
// array is, and which coordinates carry a matrix index. All of it comes from the vertex
// registers, so it holds until one is written.
typedef struct {
  unsigned written;
  Vertex start;
  Array arrays[MOST_STEPS];
  int coordinate_matrices[TEXTURES], coordinate_matrix_count;
} Prepared;
static Prepared prepared[8];

static const Prepared *prepare(int format, const Plan *plan, const Format *f) {
  Prepared *ready = &prepared[format];
  if (ready->written == gx_cp_written) return ready;
  ready->written = gx_cp_written;
  // The arrays' places are registers too, kept for element(); the general decoder sets the same.
  for (int a = 0; a < ARRAYS; a++) array_base[a] = gx.cp[CP_ARRAY_BASE + a], array_stride[a] = gx.cp[CP_ARRAY_STRIDE + a] & 0xff;
  if (f->normal_components == ALL_NORMAL_VALUES && f->normal != KIND_NONE && (f->texture[VERTEX_NORMALS_UV] || f->texture[VERTEX_NORMALS_UV + 1] || f->texture[VERTEX_NORMALS_UV + 2])) {
    debug_unsupported("vtx.normal_nbt_9_values", "texture coordinates 5 to 7 beside binormal and tangent", (uint32_t)format);
  }
  vertex_start(f, &ready->start);
  for (int k = 0; k < plan->steps_count; k++) ready->arrays[k] = array_at(array_base[plan->steps[k].array], array_stride[plan->steps[k].array]);
  ready->coordinate_matrix_count = 0;
  for (int i = 0; i < TEXTURES; i++) {
    if (f->texture_index[i]) ready->coordinate_matrices[ready->coordinate_matrix_count++] = i;
  }
  return ready;
}

static int read_vertices_planned(const Plan *plan, const Prepared *ready, const uint8_t *data, int count, Vertex *out, const Format *f) {
  const Step *steps = plan->steps;
  const int steps_count = plan->steps_count, position_index = f->position_index, coordinate_matrix_count = ready->coordinate_matrix_count;
  const uint32_t bytes = plan->bytes;
  int kept = 0;
  for (int n = 0; n < count; n++) {
    const uint8_t *vertex = data + (size_t)n * bytes;
    Vertex *v = &out[kept];
    *v = ready->start;
    if (position_index | coordinate_matrix_count) {
      uint32_t matrix_at = 0;
      if (position_index) v->matrix[VERTEX_MATRIX_POSITION] = vertex[matrix_at++] & MATRIX_INDEX_MASK;
      for (int k = 0; k < coordinate_matrix_count; k++) v->matrix[VERTEX_MATRIX_COORDINATE + ready->coordinate_matrices[k]] = vertex[matrix_at++] & MATRIX_INDEX_MASK;
    }
    int skipped = 0;
    for (int k = 0; k < steps_count; k++) {
      const Step *step = &steps[k];
      const uint8_t *at = vertex + step->at, *source;
      if (step->kind == KIND_INDEX8) {
        uint32_t index = at[0];
        if (step->array == ARRAY_POSITION) skipped = index == NULL_INDEX8;
        source = element(&ready->arrays[k], step->array, index);
      } else if (step->kind == KIND_INDEX16) {
        uint32_t index = big16(at);
        if (step->array == ARRAY_POSITION) skipped = index == NULL_INDEX16;
        source = element(&ready->arrays[k], step->array, index);
      } else {
        source = at;
      }
      if (!source) continue;
      float *to = (float *)((uint8_t *)v + step->out);
      if (step->read) {
        step->read(source, to, step->scale);
      } else {  // Normals, with the tangent and binormal of nine values after it.
        uint32_t size = component_bytes(f->normal_type);
        float *parts[3] = {v->normal, v->tangent, v->binormal};
        for (uint32_t part = 0; part < (uint32_t)f->normal_components / 3; part++) read_normal(source + part * 3 * size, f, parts[part]);
      }
    }
    if (n == count - 1) keep_last_normals(f, v);
    if (!skipped) kept++;
  }
  return kept;
}

int gx_read_vertices(const uint8_t *data, int format, int count, Vertex *out) {
  format &= 7;
  const Plan *plan = plan_for(format);
  // A display list's draw kept for reuse records which indexes it read, which only the general decoder does.
  if (!plan->ready || spans.on) return read_vertices_general(data, format, count, out);
  const Format *f = format_for(format);
  return read_vertices_planned(plan, prepare(format, plan, f), data, count, out, f);
}

// Display lists: models are drawn by calling the same lists frame after frame, and their
// vertices decode the same way each time. So each draw inside a called list keeps what
// it decoded, and the next call reuses it while three things hold: the list's bytes are
// unwritten, so the draw is the same command; the vertex registers it was decoded with are
// the same; and the arrays it indexed are unwritten (memory_watch.h). WiiCompiled keeps
// a scan of each list in the same way (CREDITS.md). WII_RECOMP_LIST_CACHE=1 turns it on.
GxCalledList gx_called_list;
unsigned gx_list_draws_reused, gx_list_draws_decoded;

enum { KEPT_SLOTS = 8192, KEPT_PROBES = 8, KEPT_BYTES_MOST = 64 << 20, VERTEX_REGISTERS = 7 };
typedef struct {
  uint32_t list, list_bytes, offset;
  uint64_t list_watched;
  uint32_t registers[VERTEX_REGISTERS], used, base[ARRAYS], stride[ARRAYS], first[ARRAYS], bytes[ARRAYS];
  uint64_t watched[ARRAYS];
  int format, count, kept, sets_normals;
  float normals[CACHED_NORMALS][4];
  Vertex *vertices;
} KeptDraw;
static KeptDraw kept_draws[KEPT_SLOTS];
static size_t kept_bytes;
static int keeping = -1;

static void vertex_registers(int format, uint32_t out[VERTEX_REGISTERS]) {
  out[0] = gx.cp[CP_DESCRIPTOR_LOW], out[1] = gx.cp[CP_DESCRIPTOR_HIGH];
  out[2] = gx.cp[CP_FORMAT_A + format], out[3] = gx.cp[CP_FORMAT_B + format], out[4] = gx.cp[CP_FORMAT_C + format];
  out[5] = gx.cp[CP_MATRIX_INDEX], out[6] = gx.cp[CP_MATRIX_INDEX + 0x10];
}

static int still_holds(const KeptDraw *d, int format, int count) {
  if (d->list != gx_called_list.address || d->list_bytes != gx_called_list.bytes || d->list_watched != gx_called_list.watched) return 0;
  if (d->format != format || d->count != count) return 0;
  uint32_t registers[VERTEX_REGISTERS];
  vertex_registers(format, registers);
  if (memcmp(registers, d->registers, sizeof registers)) return 0;
  for (int a = 0; a < ARRAYS; a++) {
    if (!(d->used & (1u << a))) continue;
    if (gx.cp[CP_ARRAY_BASE + a] != d->base[a] || (gx.cp[CP_ARRAY_STRIDE + a] & 0xff) != d->stride[a]) return 0;
    if (watch_count(d->first[a], d->bytes[a]) != d->watched[a]) return 0;
  }
  return 1;
}

static KeptDraw *kept_slot(uint32_t offset) {
  uint32_t mixed = (gx_called_list.address ^ (offset * 0x9e3779b1u)) * 2654435761u, first = mixed % KEPT_SLOTS;
  for (int step = 0; step < KEPT_PROBES; step++) {
    KeptDraw *d = &kept_draws[(first + (uint32_t)step) % KEPT_SLOTS];
    if (!d->vertices || (d->list == gx_called_list.address && d->offset == offset)) return d;
  }
  return &kept_draws[first];
}

// Keeps a draw just decoded, if everything it read can be watched.
static void keep(KeptDraw *d, uint32_t offset, int format, int count, int kept, const Vertex *out) {
  if (gx_called_list.watched == WATCH_NONE) return;
  uint32_t used = spans.used, first[ARRAYS], bytes[ARRAYS];
  uint64_t watched[ARRAYS];
  for (int a = 0; a < ARRAYS; a++) {
    if (!(used & (1u << a))) continue;
    uint32_t stride = gx.cp[CP_ARRAY_STRIDE + a] & 0xff;
    // An element is at most a stride long, or 36 bytes of floats where the stride is less.
    uint32_t element = stride > 36 ? stride : 36;
    first[a] = (gx.cp[CP_ARRAY_BASE + a] + spans.low[a] * stride) | 0x80000000u;
    bytes[a] = (spans.high[a] - spans.low[a]) * stride + element;
    watched[a] = watch_range(first[a], bytes[a]);
    if (watched[a] == WATCH_NONE) return;
  }
  size_t room = (size_t)count * sizeof(Vertex);
  if (d->vertices && d->count != count) {
    free(d->vertices);
    kept_bytes -= (size_t)d->count * sizeof(Vertex);
    d->vertices = 0;
  }
  if (!d->vertices) {
    if (kept_bytes + room > KEPT_BYTES_MOST) return;
    d->vertices = malloc(room);
    if (!d->vertices) return;
    kept_bytes += room;
  }
  memcpy(d->vertices, out, (size_t)kept * sizeof(Vertex));
  d->list = gx_called_list.address, d->list_bytes = gx_called_list.bytes, d->offset = offset;
  d->list_watched = gx_called_list.watched;
  vertex_registers(format, d->registers);
  d->used = used;
  for (int a = 0; a < ARRAYS; a++) {
    if (!(used & (1u << a))) continue;
    d->base[a] = gx.cp[CP_ARRAY_BASE + a], d->stride[a] = gx.cp[CP_ARRAY_STRIDE + a] & 0xff;
    d->first[a] = first[a], d->bytes[a] = bytes[a], d->watched[a] = watched[a];
  }
  d->format = format, d->count = count, d->kept = kept;
  d->sets_normals = formats[format].normal != KIND_NONE;
  memcpy(d->normals, gx_cached_normals, sizeof d->normals);
}

// Whether lists' draws are kept at all. When they aren't, a called list's memory needn't be watched.
int gx_lists_kept(void) {
  if (keeping < 0) keeping = watch_enabled() && getenv("WII_RECOMP_LIST_CACHE");
  return keeping;
}

int gx_read_list_vertices(const uint8_t *data, int format, int count, Vertex *out) {
  format &= 7;
  // Off unless asked for: on the Pi, half a race's list draws miss, as characters' vertices
  // are rewritten every frame, and the lookups and the extra copy of each kept vertex cost
  // more than decoding saves (a race frame's commands took 13.1 ms with it, 11.5 without).
  // Keeping the vertices on the graphics card instead, so a hit copies nothing, is what
  // would pay.
  if (!gx_lists_kept()) return gx_read_vertices(data, format, count, out);
  uint32_t offset = (uint32_t)(data - gx_called_list.start);
  KeptDraw *d = kept_slot(offset);
  if (d->vertices && d->offset == offset && still_holds(d, format, count)) {
    memcpy(out, d->vertices, (size_t)d->kept * sizeof(Vertex));
    if (d->sets_normals) memcpy(gx_cached_normals, d->normals, sizeof d->normals);
    gx_list_draws_reused++;
    return d->kept;
  }
  gx_list_draws_decoded++;
  spans.on = 1, spans.used = 0;
  int kept = gx_read_vertices(data, format, count, out);
  spans.on = 0;
  keep(d, offset, format, count, kept, out);
  return kept;
}
