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

NOTES FOR WINAMP INTRO VIDEO
-----------------------------
- surfaces cpu, textures gpu, utility for creating texture from surface (then free the surface)
- SDL_RenderCopy
- create load_texture subroutine

NOTES FOR REFACTOR VIDEO
-------------------------
- using pointer to winampskin in load_skin method bc you can't use references (&) in C,
    only way to "pass by reference" is with pointers
- SDL_arraysize macro
- draw_frame(), draw_button()
- converted_buffer array is static bc in old times was poor practice to put 2kB on the stack
*/

#include "SDL.h"
#include "physfs.h"
#include "physfsrwops.h"
#include "ignorecase.h"

typedef void (*ClickFn)(void);

// tagging struct so that it doesn't show up as unnamed in VSCode
typedef struct WinampSkinBtn {
    SDL_Texture* tex;
    SDL_Rect src_unpressed_rect;
    SDL_Rect src_pressed_rect;
    SDL_Rect dest_rect;
    ClickFn clickfn;
} WinampSkinBtn;

// tagging enum so that it doesn't show up as unnamed in VSCode
typedef enum WinampSkinBtnID {
    BTN_PREV,
    BTN_PLAY,
    BTN_PAUSE,
    BTN_STOP,
    BTN_NEXT,
    BTN_EJECT,
    BTN_TOTAL
} WinampSkinBtnID;

// sliders have a separate bitmap for each state
// tagging struct so that it doesn't show up as unnamed in VSCode
typedef struct WinampSkinSlider {
    SDL_Texture* tex;
    WinampSkinBtn knob;
    int n_frames;
    int frame_x_offset;
    int frame_y_offset;
    int frame_width;
    int frame_height;
    SDL_Rect dest_rect;
    float val;
} WinampSkinSlider;

// tagging enum so that it doesn't show up as unnamed in VSCode
typedef enum WinampSkinSliderID { SLD_VOLUME, SLD_BALANCE, SLD_TOTAL } WinampSkinSliderID;

// tagging struct so that it doesn't show up as unnamed in VSCode
typedef struct WinampSkin {
    SDL_Texture* tex_main;
    SDL_Texture* tex_cbuttons;
    SDL_Texture* tex_volume;
    SDL_Texture* tex_balance;
    WinampSkinBtn buttons[BTN_TOTAL];
    WinampSkinSlider sliders[SLD_TOTAL];
    WinampSkinBtn* pressed_btn;

} WinampSkin;

static SDL_AudioDeviceID audio_device = 0;
SDL_AudioSpec wavspec;
static SDL_Window* window = NULL;  // any time you need to draw to screen in sdl, you need a window
static SDL_Renderer* renderer = NULL;

// THIS GLOBAL STATE IS NOT PERMANAENT
// static variables in C are initialized to zero when declared
// eventually might want to put these in a struct so one "thing" is getting passed around, not a lot of individual
// globals

static SDL_AudioStream* stream = NULL;  // don't have to initialize stack variable to null?
static SDL_AudioSpec desired;
static Uint8* wavbuf = NULL;
static Uint32 wavlen = 0;

static WinampSkin skin;

static SDL_bool paused = SDL_TRUE;

static const char* physfs_errstr(void) { return PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()); }


static SDL_RWops* open_rw( const char* _fname) {
    char* fname = SDL_strdup(_fname);
    if (!fname) {
        SDL_OutOfMemory();
        return NULL;
    }
    PHYSFSEXT_locateCorrectCase(fname);
    SDL_RWops* retval = PHYSFSRWOPS_openRead(fname);
    SDL_free(fname);

    return retval;
}

#if defined(__GNUC__) || defined(__clang__)
static void panic_and_abort(const char* title, const char* text) __attribute__((noreturn));  // c/c++ attributes
#endif


static void panic_and_abort(const char* title, const char* text) {
    fprintf(stderr, "PANIC: %s ... %s\n", title, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, window);
    SDL_Quit();
    exit(1);
}

static void SDLCALL feed_audio_device_callback(void* __attribute__((unused)) userdata, Uint8* output_stream, int len) {
    // need to write data to "stream"
    // desired.samples: bigger numbers mean fewer calls, smaller numbers mean less latency - how much audio gets
    // buffered in a calls?
    int gotten_bytes = 0;
    if (stream == NULL) {
        goto fill_silence;
    }
    gotten_bytes = SDL_AudioStreamGet(stream, output_stream, len);
    if (gotten_bytes == 0) {
        goto fill_silence;
    }
    float* samples = (float*)output_stream;
    if (gotten_bytes == -1) {
        panic_and_abort("failed to get converted data", SDL_GetError());
    }

    const float volume = skin.sliders[SLD_VOLUME].val;
    const float balance = skin.sliders[SLD_BALANCE].val;
    const int n_samples = gotten_bytes / sizeof(float);  // we are using F32 samples as specified in desired.format
    SDL_assert((n_samples % 2) == 0);

    // changing volume based on volume_level here:
    for (int i = 0; i < n_samples; i++) {
        samples[i] *= volume;
    }

    if (balance > 0.5f) {  // left samples
        for (int i = 0; i < n_samples; i += 2) {
            samples[i] *= 1.0f - balance;
        }
    } else if (balance < 0.5f) {  // right samples
        for (int i = 0; i < n_samples; i += 2) {
            samples[i + 1] *= balance;
        }
    }

fill_silence:
    len -= gotten_bytes;  // how many bytes under len are we after feeding device
    output_stream += gotten_bytes;
    SDL_memset(
            output_stream, '\0', len);  // this will return immediately if len=0 so don't need to check that condition
    return;
}

static void stop_audio(void) {
    SDL_LockAudioDevice(audio_device);
    if (stream) {
        SDL_FreeAudioStream(stream);
    }
    stream = NULL;
    SDL_UnlockAudioDevice(audio_device);

    if (wavbuf) {
        SDL_FreeWAV(wavbuf);
    }
    wavbuf = NULL;
    wavlen = 0;
}

static void prev_clickfn(void) {
    // !!! FIXME: if you spam restart button takes a while after the most recent press to
    // play the audio - think this is because of multiple calls to convert the entire wav
    // file

    // both stream clear and put functions are thread-safe - don't need to lock audio device
    // when passing stream global to them
    SDL_AudioStreamClear(stream);
    SDL_AudioStreamPut(stream, wavbuf, wavlen);
    SDL_AudioStreamFlush(stream);
}

static void pause_clickfn(void) {
    paused = paused ? SDL_FALSE : SDL_TRUE;
    SDL_PauseAudioDevice(audio_device, paused);
}

static void stop_clickfn(void) { stop_audio(); }

// inlined funtion?
static SDL_INLINE void init_skin_btn(
        WinampSkinBtn* btn,
        SDL_Texture* tex,
        ClickFn clickfn,
        const SDL_Rect src_unpressed_rect,
        const SDL_Rect src_pressed_rect,
        const SDL_Rect dest_rect) {
    btn->tex = tex;
    btn->clickfn = clickfn;
    btn->src_unpressed_rect = src_unpressed_rect;
    btn->src_pressed_rect = src_pressed_rect;
    btn->dest_rect = dest_rect;
}

// this and above button init method are kind of like constructors in C
static SDL_INLINE void init_skin_slider(
        WinampSkinSlider* slider,
        SDL_Texture* tex,
        const SDL_Rect knob_src_unpressed_rect,
        const SDL_Rect knob_src_pressed_rect,
        const int frame_x_offset,
        const int frame_y_offset,
        const int n_frames,
        const int frame_width,
        const int frame_height,
        const SDL_Rect dest_rect,
        const float val) {
    slider->tex = tex;
    slider->frame_x_offset = frame_x_offset;
    slider->frame_y_offset = frame_y_offset;
    slider->frame_width = frame_width;
    slider->frame_height = frame_height;
    slider->n_frames = n_frames;
    slider->val = val;
    slider->dest_rect = dest_rect;

    const int knob_dest_x = (int)dest_rect.x + (slider->val * dest_rect.w) - (knob_src_unpressed_rect.w / 2);
    const int min_knob_dest_x = slider->dest_rect.x;
    const int max_knob_dest_x = slider->dest_rect.x + slider->dest_rect.w - knob_src_unpressed_rect.w;
    const int clamped_knob_dest_x = SDL_clamp(knob_dest_x, min_knob_dest_x, max_knob_dest_x);
    const int knob_dest_y = dest_rect.y - (knob_src_pressed_rect.h - dest_rect.h) / 2;
    const SDL_Rect knob_dest_rect
            = {clamped_knob_dest_x, knob_dest_y, knob_src_unpressed_rect.w, knob_src_unpressed_rect.h};
    init_skin_btn(&slider->knob, tex, NULL, knob_src_unpressed_rect, knob_src_pressed_rect, knob_dest_rect);
}

static SDL_Texture* load_texture(SDL_RWops* rw) {
    if (rw == NULL) {
        return NULL;
    }
    SDL_Surface* surface = SDL_LoadBMP_RW(rw, 1);
    if (!surface) {
        return NULL;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;  // may be NULL
}

static void free_skin(WinampSkin* skin) {
    if (skin->tex_main) {
        SDL_DestroyTexture(skin->tex_main);
    }
    if (skin->tex_cbuttons) {
        SDL_DestroyTexture(skin->tex_cbuttons);
    }
    if (skin->tex_volume) {
        SDL_DestroyTexture(skin->tex_volume);
    }
    if (skin->tex_balance) {
        SDL_DestroyTexture(skin->tex_balance);
    }

    SDL_zerop(skin);  // zerop lets you pass in pointer instead of dereferenced ptr
}

static void load_skin(WinampSkin* skin, const char* __attribute__((unused)) fname) {

    free_skin(skin);
    if (!PHYSFS_mount(fname, NULL, 1)) {
        return; // ok if can't load from file
    }

    skin->tex_main = load_texture(open_rw("Main.bmp"));
    skin->tex_cbuttons = load_texture(open_rw("CButtons.bmp"));
    skin->tex_volume = load_texture(open_rw("Volume.bmp"));
    skin->tex_balance = load_texture(open_rw("Balance.bmp"));

    PHYSFS_unmount(fname);

    skin->pressed_btn = NULL;

    init_skin_btn(
            &(skin->buttons[BTN_PREV]),
            skin->tex_cbuttons,
            &prev_clickfn,
            (SDL_Rect){0, 0, 23, 18},
            (SDL_Rect){0, 18, 23, 18},
            (SDL_Rect){16, 88, 23, 18});
    init_skin_btn(
            &(skin->buttons[BTN_PLAY]),
            skin->tex_cbuttons,
            NULL,
            (SDL_Rect){23, 0, 23, 18},
            (SDL_Rect){23, 18, 23, 18},
            (SDL_Rect){39, 88, 23, 18});
    init_skin_btn(
            &(skin->buttons[BTN_PAUSE]),
            skin->tex_cbuttons,
            &pause_clickfn,
            (SDL_Rect){46, 0, 23, 18},
            (SDL_Rect){46, 18, 23, 18},
            (SDL_Rect){62, 88, 23, 18});
    init_skin_btn(
            &(skin->buttons[BTN_STOP]),
            skin->tex_cbuttons,
            &stop_clickfn,
            (SDL_Rect){69, 0, 23, 18},
            (SDL_Rect){69, 18, 23, 18},
            (SDL_Rect){85, 88, 23, 18});
    init_skin_btn(
            &(skin->buttons[BTN_NEXT]),
            skin->tex_cbuttons,
            NULL,
            (SDL_Rect){92, 0, 22, 18},
            (SDL_Rect){92, 18, 22, 18},
            (SDL_Rect){108, 88, 22, 18});
    init_skin_btn(
            &(skin->buttons[BTN_EJECT]),
            skin->tex_cbuttons,
            NULL,
            (SDL_Rect){114, 0, 22, 16},
            (SDL_Rect){114, 16, 22, 16},
            (SDL_Rect){136, 89, 22, 16});

    init_skin_slider(
            &skin->sliders[SLD_VOLUME],
            skin->tex_volume,
            (SDL_Rect){0, 422, 14, 11},
            (SDL_Rect){15, 422, 14, 11},
            0,
            0,
            28,
            68,
            15,
            (SDL_Rect){107, 57, 68, 13},
            1.0f);  // volume level starts at 1.0

    init_skin_slider(
            &skin->sliders[SLD_BALANCE],
            skin->tex_balance,
            (SDL_Rect){0, 422, 14, 11},
            (SDL_Rect){15, 422, 14, 11},
            9,   // x offset
            0,   // y offset
            28,  // n frames
            38,  // frame width
            15,  // frame height
            (SDL_Rect){177, 57, 38, 13},
            0.5f);  // balance level starts at 0.5
}

static SDL_bool load_wav_to_stream(char* fname) {

    /* deal with wav data */
    SDL_FreeWAV(wavbuf);  // if you pass SDL_free a null it's a noop
    wavbuf = NULL;
    wavlen = 0;  // length in BYTES
    SDL_AudioSpec wavspec;
    if (SDL_LoadWAV(fname, &wavspec, &wavbuf, &wavlen) == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't load wav file", SDL_GetError(), window);
        goto failed;
    }

    /* deal with stream */
    SDL_AudioStream* tmpstream = stream;  // set tmp var so we can free stream outside of locked code
    // lock the audio device here so the callback can't touch "stream" global between lock/unlock
    SDL_LockAudioDevice(audio_device);
    stream = NULL;
    SDL_UnlockAudioDevice(audio_device);

    // free the stream pointer first in case it already exists so you don't leak memory
    // also using tmpstream temp var until the stream is setup before re-setting global "stream" ptr val
    // (don't want to be working with stream global when callback isn't locked)
    SDL_FreeAudioStream(tmpstream);
    tmpstream = SDL_NewAudioStream(
            wavspec.format, wavspec.channels, wavspec.freq, desired.format, desired.channels, desired.freq);
    if (!tmpstream) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "couldn't create audio stream", SDL_GetError(), window);
        goto failed;
    }

    // error condition can happen if you run out of memory
    if (SDL_AudioStreamPut(tmpstream, wavbuf, wavlen) == -1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "couldn't put data in audio stream", SDL_GetError(), window);
        goto failed;
    }

    if (SDL_AudioStreamFlush(tmpstream) == -1) {  // convert all of the  data so it's available with SDL_AudioStreamGet
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "couldn't flush audio stream", SDL_GetError(), window);
        goto failed;
    }

    SDL_LockAudioDevice(audio_device);
    stream = tmpstream;
    SDL_UnlockAudioDevice(audio_device);

    return SDL_TRUE;

failed:
    // printf("in faliled section\n");
    stop_audio();
    return SDL_FALSE;
}
// FIXME!!! can have this function return an audio stream and get rid of the stream global?

static void init_everything(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == -1) {
        panic_and_abort("SDL_Init failed", SDL_GetError());
    }

    if (!PHYSFS_init(argv[0])) {
        panic_and_abort("PHYSFS_init failed", physfs_errstr());
    }

    // tells sdl we want this event type enabled it's disabled by default bc dropfile event triggers
    // dynamic allocation of char* - memory will leak unless we free it explicitly with SDL_free when
    // we're done with it
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    window = SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 275, 116, 0);
    if (!window) {
        panic_and_abort("SDL_CreateWindow failed", SDL_GetError());
    }

    // renderer overlays things on created window - create opengl context?
    // talk to the render in simple primitives
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        panic_and_abort("SDL_CreateRenderer failed", SDL_GetError());
    }
    load_skin(&skin, "skinner_atlas.wsz");

    SDL_zero(desired);
    desired.freq = 48000;
    desired.format = AUDIO_F32;
    desired.channels = 2;
    desired.samples = 4096;
    desired.callback = feed_audio_device_callback;

    // 2nd null arg is for obtained audiospec param, telling sdl that if hardware has issues with desired spec, make SDL
    // "fake" desired spec so that we can write code for desired spec (HAS to work with desired spec)
    audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (audio_device == 0) {
        panic_and_abort("Couldn't open audio device", SDL_GetError());
    }

    load_wav_to_stream("music.wav");
}

static void deinit_everything() {
    free_skin(&skin);
    SDL_FreeWAV(wavbuf);
    SDL_CloseAudioDevice(audio_device);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    PHYSFS_deinit();

    SDL_Quit();
}

static void draw_button(SDL_Renderer* renderer, WinampSkinBtn* btn) {
    const SDL_bool pressed = skin.pressed_btn == btn;
    if (btn->tex == NULL) {
        if (pressed) {
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        }
        SDL_RenderFillRect(renderer, &(btn->dest_rect));
        return;
    }
    SDL_RenderCopy(renderer, btn->tex, pressed ? &btn->src_pressed_rect : &btn->src_unpressed_rect, &btn->dest_rect);
}

static void draw_slider(SDL_Renderer* renderer, WinampSkinSlider* slider) {
    SDL_assert(slider->val >= 0.0f);
    SDL_assert(slider->val <= 1.0f);

    const SDL_bool pressed = skin.pressed_btn == &slider->knob;
    // draw rects if no texture available
    if (slider->tex == NULL) {
        const int color = (int)(255.0f * slider->val);
        SDL_SetRenderDrawColor(renderer, color, color, color, 255);
        SDL_RenderFillRect(renderer, &slider->dest_rect);
        if (pressed) {
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        }
        SDL_RenderFillRect(renderer, &slider->knob.dest_rect);
        return;
    }
    const int frame_idx = (int)(slider->val * (slider->n_frames - 1));
    const int src_y = slider->frame_y_offset + frame_idx * slider->frame_height;
    const SDL_Rect src_rect = {slider->frame_x_offset, src_y, slider->dest_rect.w, slider->dest_rect.h};
    SDL_RenderCopy(renderer, slider->tex, &src_rect, &slider->dest_rect);
    if (pressed) {
        SDL_RenderCopy(renderer, slider->tex, &slider->knob.src_pressed_rect, &slider->knob.dest_rect);
    } else {
        SDL_RenderCopy(renderer, slider->tex, &slider->knob.src_unpressed_rect, &slider->knob.dest_rect);
    }
}
static void draw_frame(SDL_Renderer* renderer, WinampSkin* skin) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_RenderCopy(renderer, skin->tex_main, NULL, NULL);

    for (int i = 0; i < (int)SDL_arraysize(skin->buttons); i++) {
        draw_button(renderer, &skin->buttons[i]);
    }
    draw_slider(renderer, &skin->sliders[SLD_VOLUME]);
    draw_slider(renderer, &skin->sliders[SLD_BALANCE]);

    SDL_RenderPresent(renderer);
}

static void handle_slider_motion(WinampSkinSlider* slider, const SDL_Point* pt) {
    // !!! FIXME: having the point in rect as an additional condition feels not very elegant, see if you can clean up
    // conditions it's done this way though to make sure that slider vals don't get updated with out-of-rect mouse
    // positions, since the knob can still be pressed when the mouse is outside of the rect if the mouse is being held
    // down
    if (skin.pressed_btn == &slider->knob) {
        const float new_val = (float)(pt->x - slider->dest_rect.x) / slider->dest_rect.w;

        const int new_knob_x = pt->x - (slider->knob.dest_rect.w / 2);
        const int min_knob_x = slider->dest_rect.x;
        const int max_knob_x = slider->dest_rect.x + slider->dest_rect.w
                               - slider->knob.dest_rect.w;  // want to pre-calc this before feeding into macro
        slider->knob.dest_rect.x = SDL_clamp(new_knob_x, min_knob_x, max_knob_x);
        SDL_LockAudioDevice(audio_device);
        slider->val = SDL_clamp(new_val, 0.0f, 1.0f);
        SDL_UnlockAudioDevice(audio_device);
    }
}

static SDL_bool handle_events(WinampSkin* skin) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: {
                return SDL_FALSE;
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                // we only care about left clicking
                if (e.button.button != SDL_BUTTON_LEFT) {
                    break;
                }

                const SDL_Point pt = {e.button.x, e.button.y};
                if (skin->pressed_btn == NULL) {
                    for (int i = 0; i < (int)SDL_arraysize(skin->buttons); i++) {
                        WinampSkinBtn* btn = &skin->buttons[i];
                        if (SDL_PointInRect(&pt, &btn->dest_rect)) {
                            skin->pressed_btn = btn;
                            break;
                        }
                    }
                    for (int i = 0; i < (int)SDL_arraysize(skin->sliders); i++) {
                        WinampSkinSlider* slider = &skin->sliders[i];
                        if (SDL_PointInRect(&pt, &slider->dest_rect)) {
                            skin->pressed_btn = &slider->knob;
                            break;
                        }
                    }
                }

                // captures mouse so that while it's pressed, sliders can change value while mouse is outside of window
                if (skin->pressed_btn) {
                    SDL_CaptureMouse(SDL_TRUE);
                }
                break;
            }

            case SDL_MOUSEBUTTONUP: {
                if (e.button.button != SDL_BUTTON_LEFT) {
                    break;
                }

                if (skin->pressed_btn) {
                    // release mouse if it was captured on mousebtnup (only would have been captured if pressed_btn was
                    // set to non-null val, so can uncapture in this if statement)
                    SDL_CaptureMouse(SDL_FALSE);
                    if (skin->pressed_btn->clickfn) {

                        // only call button's clickfn if mouse is released while inside button's rect
                        const SDL_Point pt = {e.button.x, e.button.y};
                        if (SDL_PointInRect(&pt, &skin->pressed_btn->dest_rect)) {
                            skin->pressed_btn->clickfn();
                        }
                    }
                    skin->pressed_btn = NULL;
                }
                break;
            }

            case SDL_MOUSEMOTION: {
                const SDL_Point pt = {e.motion.x, e.motion.y};
                for (int i = 0; i < (int)SDL_arraysize(skin->sliders); i++) {
                    handle_slider_motion(&skin->sliders[i], &pt);
                }
                break;
            }

            case SDL_DROPFILE: {
                const char* ptr = SDL_strrchr(e.drop.file, '.');
                if ((ptr && SDL_strcasecmp(ptr, ".wsz") == 0) || (SDL_strcasecmp(ptr, ".zip") == 0)) {
                    load_skin(skin, e.drop.file);
                } else {
                    load_wav_to_stream(e.drop.file);
                }
                SDL_free(e.drop.file);
                break;
            }
        }
    }
    return SDL_TRUE;
}

int main(int argc, char** argv) {
    init_everything(argc, argv);  // will panic and abort on fail
    while (handle_events(&skin)) {
        draw_frame(renderer, &skin);
    }

    deinit_everything();
    return 0;
}

// feed audio samples/data to the audio device
// load in wav at startup and when new file is selected (drag n drop for now)
// have audio play from device based on state of pause button
// restart current file when restart button pressed (will play or be paused based on pause button state

/*
now, the "pressed" state info is held in the slider struct
there should only be one button pressed at a time, so that way of storing that state naturally aligns with that
requirement

on mousebuttondown: if mouse is within a button's bounds, AND the pressed_btn is currently NULL, set the pressed_btn to
that button on mousebuttonup: set the pressed_btn to NULL

if I click on button x, then drag mouse to another button, button x will remain the pressed one until mousebtnup




*/
