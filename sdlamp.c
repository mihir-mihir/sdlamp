/*
NOTES FOR POST 16:20 AUDIO STREAMS VIDEO
-----------------------------------------
- general idea: implementing volume control involves making changes to the audio data in real time
- going to feed audio device a little bit at a time instead of dumping the entire audio file in the queue
- manipulating the audio will get complicated if allowed to use a lot of different formats,
    fix a single format to work with
- SDL_GetQueuedAudioSize -- how many bytes have I given to SDL to give to the audio device by calling SDL_QueueAudio
- if less than 8 kB that we've given, need to feed more data (give 32 kB at a time for now? or however much is left if
less)
- SDL_assert
- rewind button logic (clear queue, update wavpos), pause button logic
- SDL_AudioStream global - init to NULL to be explicit
    - this is how we "fake" desired audiospec?
    - now only the stream needs to be global

NOTES FOR VOLUME CONTROL VIDEO
--------------------------------
- sdl mouse motion event
- sample frames - 2 samples make up a frame in stereo, we are just going to work on all samples without worrying about
frames
- in converted_buf, each sample is a float (4 bytes) - remember converted_buf is type Uint8* though, so need to cast to
float* to go sample by sample in for loop


NOTES FOR BALANCE CONTROL VIDEO (5)
------------------------------------
- goto failed (gotos in c vs c++)
- stop audio instead of exiting in some cases?

NOTES  FOR WINAMP INTRO VIDEO
- surfaces cpu, textures gpu, utility for creating texture from surface (then free the surface)
- SDL_RenderCopy

*/



#include "SDL.h"

static SDL_AudioDeviceID audio_device = 0;
SDL_AudioSpec wavspec;
static SDL_Window* window = NULL;  // any time you need to draw to screen in sdl, you need a window
static SDL_Renderer* renderer = NULL;

#if defined(__GNUC__) || defined(__clang__)
static void panic_and_abort(const char* title, const char* text) __attribute__((noreturn));  // c/c++ attributes
#endif

static void panic_and_abort(const char* title, const char* text) {
    fprintf(stderr, "PANIC: %s ... %s\n", title, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, window);
    SDL_Quit();
    exit(1);
}

// THIS GLOBAL STATE IS NOT PERMANAENT
// static variables in C are initialized to zero when declared
// eventually might want to put these in a struct so one "thing" is getting passed around, not a lot of individual
// globals

static SDL_AudioStream* stream = NULL;  // don't have to initialize stack variable to null?
static SDL_AudioSpec desired;
static Uint8* wavbuf = NULL;
static Uint32 wavlen = 0;

// !!! FIXME: you don't need a global for this, can use static Uint8 cb* inside of event loop - using "static"
// ensures that memory is only allocated for it once, so you're not reallocating every time the loop runs
// one instance of the variable is shared across all calls to the function that the static variable is declared in
static Uint8* converted_buf[32 * 1024];  // 32 kB

float volume_level = 1.0f;

// FIXME!!! can have this function return an audio stream and get rid of the stream global?
static SDL_bool init_audio_stream(char* fname) {

    // free the stream pointer first in case it already exists so you don't leak memory
    SDL_FreeAudioStream(stream);
    stream = NULL;
    SDL_AudioSpec wavspec;
    SDL_FreeWAV(wavbuf);  // if you pass SDL_free a null it's a noop
    wavbuf = NULL;
    wavlen = 0;  // length in BYTES
    if (SDL_LoadWAV(fname, &wavspec, &wavbuf, &wavlen) == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't load wav file", SDL_GetError(), window);
        return SDL_FALSE;
    }

    stream = SDL_NewAudioStream(
            wavspec.format, wavspec.channels, wavspec.freq, desired.format, desired.channels, desired.freq);

    // FIXME!!! error checking (if !stream)
    return SDL_TRUE;
}

static SDL_bool put_wavbuf_in_stream() {
    SDL_AudioStreamClear(stream);

    // error condition can happen if you run out of memory
    if (SDL_AudioStreamPut(stream, wavbuf, wavlen) == -1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't put data in audio stream", SDL_GetError(), window);
        return SDL_FALSE;
        // maybe panic and abort here? bc you want to exit if you run out of memory
    }

    // !!! FIXME: error handling
    SDL_AudioStreamFlush(stream);  // convert all of the  data so it's available with SDL_AudioStreamGet
    return SDL_TRUE;
}

static void send_audio_to_device_queue_from_stream() {
    // in the video there's a var new_bytes = SDL_min(SDL_AudioStreamAvailable(stream), sizeof(converted_buf))
    // might not need this var though bc len parameter in SDL_AudioStreamGet is specified as "max bytes to fill",
    // not exact bytes to fill, so if there are fewer bytes remaining in the stream than the number of bytes in the
    // converted buffer, it should still be ok to give sizeof(converted_buf) as the arg for the len param of
    // SDL_AudioStreamGet (good to know about SDL_min function though)

    // it looks like we do need to use "gotten_bytes" to pass into SDL_QueueAudio function
    int gotten_bytes = SDL_AudioStreamGet(stream, converted_buf, sizeof(converted_buf));
    if (gotten_bytes == -1) {
        panic_and_abort("failed to get converted data", SDL_GetError());
    }
    float* converted_samples = (float*)converted_buf;
    int nsamples = gotten_bytes / sizeof(float);
    // changing volume based on volume_level here:
    for (int i = 0; i < nsamples; i++) {
        converted_samples[i] *= volume_level;
    }
    SDL_QueueAudio(audio_device, converted_buf, gotten_bytes);
}
int main() {
    SDL_bool paused = SDL_TRUE;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == -1) {
        panic_and_abort("SDL_Init failed", SDL_GetError());
    }

    // tells sdl we want this event type enabled it's disabled by default bc dropfile event triggers
    // dynamic allocation of char* - memory will leak unless we free it explicitly with SDL_free when
    // we're done with it
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    window = SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, 0);
    if (!window) {
        panic_and_abort("SDL_CreateWindow failed", SDL_GetError());
    }

    // renderer overlays things on created window - create opengl context?
    // talk to the render in simple primitives
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        panic_and_abort("SDL_CreateRenderer failed", SDL_GetError());
    }

    SDL_zero(desired);
    desired.freq = 48000;
    desired.format = AUDIO_F32;
    desired.channels = 2;
    desired.samples = 4096;
    desired.callback = NULL;

    init_audio_stream("music.wav");
    put_wavbuf_in_stream();

    // 2nd null arg is for obtained audiospec param, telling sdl that if hardware has issues with desired spec, make SDL
    // "fake" desired spec so that we can write code for desired spec (HAS to work with desired spec)
    audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);

    if (audio_device == 0) {
        panic_and_abort("Couldn't open audio device", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(audio_device, paused);
    }

    const SDL_Rect rewind_rect = {110, 100, 100, 100};
    const SDL_Rect pause_rect = {430, 100, 100, 100};
    const SDL_Rect volume_rect = {70, 400, 500, 20};

    // no copy constructor?
    SDL_Rect volume_knob;
    SDL_memcpy(&volume_knob, &volume_rect, sizeof(SDL_Rect));
    volume_knob.w = 10;
    volume_knob.x = volume_rect.x + volume_level * volume_rect.w - (volume_knob.w / 2);

    int green = 0;
    SDL_bool keep_going = SDL_TRUE;
    while (keep_going) {
        int queued_bytes = SDL_GetQueuedAudioSize(audio_device);
        if (queued_bytes < 8192) {
            send_audio_to_device_queue_from_stream();
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT: {
                    keep_going = SDL_FALSE;
                    break;
                }

                case SDL_MOUSEBUTTONDOWN: {
                    const SDL_Point pt = {e.button.x, e.button.y};
                    if (SDL_PointInRect(&pt, &rewind_rect)) {
                        // !!! FIXME: if you spam restart button takes a while after the most recent press to
                        // play the audio - think this is because of multiple calls to convert the entire wav file
                        SDL_ClearQueuedAudio(audio_device);
                        put_wavbuf_in_stream();

                    } else if (SDL_PointInRect(&pt, &pause_rect)) {
                        paused = paused ? SDL_FALSE : SDL_TRUE;
                        if (audio_device) {
                            SDL_PauseAudioDevice(audio_device, paused);
                        }
                    }
                    break;
                }

                case SDL_MOUSEMOTION: {
                    const SDL_Point pt = {e.motion.x, e.motion.y};
                    if (e.motion.state == SDL_PRESSED && SDL_PointInRect(&pt, &volume_rect)) {
                        volume_level = (float)(e.motion.x - volume_rect.x) / volume_rect.w;
                        volume_knob.x = pt.x - (volume_knob.w / 2);
                        printf("mouse motion at (%d, %d), percent: %f\n", e.motion.x, e.motion.y, volume_level);
                    }
                    break;  // without break here, i get bad access exception on loadwav above - running out of memory?
                }

                case SDL_DROPFILE: {
                    init_audio_stream(e.drop.file);
                    put_wavbuf_in_stream();
                    SDL_free(e.drop.file);
                    break;
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, green, 0, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

        SDL_RenderFillRect(renderer, &rewind_rect);
        SDL_RenderFillRect(renderer, &pause_rect);
        SDL_RenderFillRect(renderer, &volume_rect);
        
        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        SDL_RenderFillRect(renderer, &volume_knob);

        SDL_RenderPresent(renderer);

        green = (green + 1) % 256;
    }

    SDL_FreeWAV(wavbuf);
    SDL_CloseAudioDevice(audio_device);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;
}
