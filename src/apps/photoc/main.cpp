#include "codecs/codecs.h"
#include "engine/session.h"
#include "io/io.h"
#include "ui/app.h"
#include "ui/icon_data.h"
#include "ui/icons.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

extern const unsigned char pc_app_icon[], pc_app_icon_end[];

/* The taskbar and title bar icon, decoded from the embedded PNG. */
void set_window_icon (SDL_Window* window)
{
    codecs::Image png, rgba;
    char err[256];
    if (!codecs::png_decode (
            pc_app_icon, (size_t) (pc_app_icon_end - pc_app_icon), codecs::Limits::defaults (), &png, err, sizeof err)
        || !codecs::image_convert (png, codecs::ChannelLayout::Rgba, codecs::Sample::U8, &rgba, err, sizeof err))
    {
        return;
    }
    SDL_Surface* icon = SDL_CreateSurfaceFrom (
        (int) rgba.width, (int) rgba.height, SDL_PIXELFORMAT_RGBA32, rgba.data.data (), (int) rgba.row_bytes ());
    if (icon == NULL)
    {
        return;
    }
    SDL_SetWindowIcon (window, icon);
    SDL_DestroySurface (icon);
}

/* --screenshot: the window's pixels as a PNG (a development aid for checking the look). */
bool save_screenshot (const char* path, int w, int h)
{
    codecs::Image img;
    img.width   = (uint32_t) w;
    img.height  = (uint32_t) h;
    img.layout  = codecs::ChannelLayout::Rgba;
    img.sample  = codecs::Sample::U8;
    img.has_dpi = false;
    img.data.resize ((size_t) w * h * 4);
    std::vector<uint8_t> flipped (img.data.size ());
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadBuffer (GL_BACK);
    glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data ());
    for (int y = 0; y < h; y++)
    {
        memcpy (&img.data[(size_t) y * w * 4], &flipped[(size_t) (h - 1 - y) * w * 4], (size_t) w * 4);
    }
    std::vector<uint8_t> bytes;
    char err[256];
    if (!codecs::encode (img, codecs::Format::Png, codecs::EncodeOptions::defaults (), &bytes, err, sizeof err)
        || !io::write_file (path, bytes, err, sizeof err))
    {
        fprintf (stderr, "error: cannot save the screenshot: %s\n", err);
        return false;
    }
    return true;
}

/* The desktop editor: an SDL3 window (X11 for now) with an OpenGL 3.3 core context, Dear ImGui
   on top, and every action going through the engine session. Usage: photoc [image file] */
/* --icon-sheet: every interface icon at three sizes on one image (a development aid). */
int icon_sheet (const char* path)
{
    const int sizes[3] = {16, 20, 32}, cell = 40, cols = 12;
    int rows = (int) ((ui::ICON_SOURCE_COUNT + cols - 1) / cols);
    int w = cols * cell * 3, h = rows * cell;
    codecs::Image img;
    img.width   = (uint32_t) w;
    img.height  = (uint32_t) h;
    img.layout  = codecs::ChannelLayout::Rgba;
    img.sample  = codecs::Sample::U8;
    img.has_dpi = false;
    img.data.assign ((size_t) w * h * 4, 255);
    for (size_t i = 0; i < ui::ICON_SOURCE_COUNT; i++)
    {
        for (int s = 0; s < 3; s++)
        {
            std::vector<uint8_t> a;
            if (!ui::rasterize_icon (ui::ICON_SOURCES[i].svg, sizes[s], &a))
            {
                continue;
            }
            int ox = (int) (i % cols) * cell * 3 + s * cell + 4, oy = (int) (i / cols) * cell + 4;
            for (int y = 0; y < sizes[s]; y++)
            {
                for (int x = 0; x < sizes[s]; x++)
                {
                    uint8_t* p = &img.data[((size_t) (oy + y) * w + (size_t) (ox + x)) * 4];
                    uint8_t v  = (uint8_t) (255 - a[(size_t) y * sizes[s] + x]);
                    p[0] = p[1] = p[2] = v;
                }
            }
        }
    }
    std::vector<uint8_t> bytes;
    char err[256];
    if (!codecs::encode (img, codecs::Format::Png, codecs::EncodeOptions::defaults (), &bytes, err, sizeof err)
        || !io::write_file (path, bytes, err, sizeof err))
    {
        fprintf (stderr, "error: %s\n", err);
        return 1;
    }
    return 0;
}

int main (int argc, char** argv)
{
    if (argc > 2 && strcmp (argv[1], "--icon-sheet") == 0)
    {
        return icon_sheet (argv[2]);
    }
    const char* screenshot = NULL;
    if (argc > 2 && strcmp (argv[1], "--screenshot") == 0)
    {
        screenshot = argv[2];
        argv += 2;
        argc -= 2;
    }
    /* --exec <command> <json params> (repeatable, after the screenshot path): commands run after
       opening, to set a scene up. */
    std::vector<std::pair<std::string, std::string>> execs;
    while (argc > 3 && strcmp (argv[1], "--exec") == 0)
    {
        execs.push_back (std::make_pair (std::string (argv[2]), std::string (argv[3])));
        argv += 3;
        argc -= 3;
    }
#ifdef __linux__
    /* Linux runs on X11 only (XWayland on a Wayland desktop); elsewhere SDL has no x11 driver and
       would fail to start. */
    SDL_SetHint (SDL_HINT_VIDEO_DRIVER, "x11");
    /* SDL asks the compositor to stop compositing its windows (for games); for an editor that
       opens dialogs over itself, GNOME then spends seconds switching compositing back on each
       time one appears. */
    SDL_SetHint (SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    if (!SDL_Init (SDL_INIT_VIDEO))
    {
        fprintf (stderr, "error: cannot start the display: %s\n", SDL_GetError ());
        return 1;
    }
    SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute (SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute (SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* window = SDL_CreateWindow (
        "PhotoC", 1440, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == NULL)
    {
        fprintf (stderr, "error: cannot open a window: %s\n", SDL_GetError ());
        SDL_Quit ();
        return 1;
    }
    set_window_icon (window);
    SDL_GLContext gl = SDL_GL_CreateContext (window);
    if (gl == NULL)
    {
        fprintf (stderr, "error: cannot create an OpenGL 3.3 context: %s\n", SDL_GetError ());
        SDL_DestroyWindow (window);
        SDL_Quit ();
        return 1;
    }
    SDL_GL_MakeCurrent (window, gl);
    SDL_GL_SetSwapInterval (1);

    IMGUI_CHECKVERSION ();
    ImGui::CreateContext ();
    ImGuiIO& io = ImGui::GetIO ();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    /* The panel layout lives in ~/.local/share/storyteller/photoc/, not wherever the app was
       started from. Without a writable directory it simply isn't saved. */
    std::string layout_path;
    char* pref = SDL_GetPrefPath ("storyteller", "photoc");
    if (pref != NULL)
    {
        layout_path = std::string (pref) + "layout.ini";
        SDL_free (pref);
    }
    io.IniFilename = layout_path.empty () || screenshot != NULL ? NULL : layout_path.c_str ();
    ImGui_ImplSDL3_InitForOpenGL (window, gl);
    ImGui_ImplOpenGL3_Init ("#version 330");

    {
        engine::Session session;
        /* PHOTOC_TIMING=1 logs how long commands and file operations take, on stderr. */
        const char* timing = getenv ("PHOTOC_TIMING");
        session.timing     = timing != NULL && strcmp (timing, "0") != 0 && timing[0] != 0;
        ui::App app (session, window);
        for (int i = 1; i < argc; i++)
        {
            app.open_file (argv[i]);
        }
        for (size_t i = 0; i < execs.size (); i++)
        {
            engine::Value params = engine::Value::parse (execs[i].second, NULL, false);
            if (params.is_discarded ())
            {
                params = engine::Value::object ();
            }
            app.run_command (execs[i].first.c_str (), params);
        }

        bool running = true;
        int frames   = 0;
        while (running)
        {
            /* Without input focus (a file dialog or another app in front), or from the moment a
               file dialog is asked for, the window waits for events and draws a few frames a
               second at most, enough for the marching ants: redrawing at the display's rate
               under another window keeps the compositor busy and makes that window slow. */
            SDL_Event e;
            bool focused = (SDL_GetWindowFlags (window) & SDL_WINDOW_INPUT_FOCUS) != 0 && !app.waiting_on_dialog ();
            bool have    = focused ? SDL_PollEvent (&e) : SDL_WaitEventTimeout (&e, 250);
            while (have)
            {
                ImGui_ImplSDL3_ProcessEvent (&e);
                if (e.type == SDL_EVENT_QUIT)
                {
                    running = false;
                }
                if (e.type == SDL_EVENT_CLIPBOARD_UPDATE)
                {
                    app.clipboard_changed (e.clipboard.owner);
                }
                if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID (window))
                {
                    running = false;
                }
                have = SDL_PollEvent (&e);
            }
            if (SDL_GetWindowFlags (window) & SDL_WINDOW_MINIMIZED)
            {
                SDL_Delay (10);
                continue;
            }
            ImGui_ImplOpenGL3_NewFrame ();
            ImGui_ImplSDL3_NewFrame ();
            ImGui::NewFrame ();
            app.frame ();
            if (app.quit_requested ())
            {
                running = false;
            }
            ImGui::Render ();
            int w, h;
            SDL_GetWindowSizeInPixels (window, &w, &h);
            glViewport (0, 0, w, h);
            glClearColor (0.15f, 0.15f, 0.15f, 1.0f);
            glClear (GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData (ImGui::GetDrawData ());
            if (screenshot != NULL && ++frames == 20)
            {
                save_screenshot (screenshot, w, h);
                running = false;
            }
            SDL_GL_SwapWindow (window);
        }
    }

    ImGui_ImplOpenGL3_Shutdown ();
    ImGui_ImplSDL3_Shutdown ();
    ImGui::DestroyContext ();
    SDL_GL_DestroyContext (gl);
    SDL_DestroyWindow (window);
    SDL_Quit ();
    return 0;
}
