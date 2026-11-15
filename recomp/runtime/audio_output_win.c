// Sound output on Windows, through SDL3's default playback device, which the console
// already links for controllers. SDL converts from the game's sample rate and asks for
// samples on a thread of its own.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "audio.h"

#define MOST_SAMPLES 4096

static void SDLCALL fill(void *unused, SDL_AudioStream *stream, int additional, int total) {
  (void)unused; (void)total;
  static int16_t samples[MOST_SAMPLES * AUDIO_CHANNELS];
  int wanted = additional / (int)(AUDIO_CHANNELS * sizeof(int16_t));
  while (wanted > 0) {
    int now = wanted > MOST_SAMPLES ? MOST_SAMPLES : wanted;
    audio_pull(samples, (uint32_t)now);
    SDL_PutAudioStreamData(stream, samples, now * (int)(AUDIO_CHANNELS * sizeof(int16_t)));
    wanted -= now;
  }
}

int audio_output_open(void) {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    fprintf(stderr, "sound device: %s\n", SDL_GetError());
    return 0;
  }
  // A short device buffer, as the mixer's own steps are 3 ms.
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
  SDL_AudioSpec format = {SDL_AUDIO_S16LE, AUDIO_CHANNELS, AUDIO_SAMPLE_RATE};
  SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &format, fill, 0);
  if (!stream) {
    fprintf(stderr, "sound device: %s\n", SDL_GetError());
    return 0;
  }
  return SDL_ResumeAudioStreamDevice(stream);
}
