#include "node.hpp"

#include "format.pb.h"
#include "format.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <variant>
#include <gsl/span>

#include <thread>

#include <fstream>
#include <sstream>


#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl2.h"
#include <stdio.h>
#include <SDL.h>
#include <SDL_opengl.h>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

using namespace c3::kademlia;
using namespace std::chrono_literals;

std::string bind_address;

constexpr int default_port = 5021;

class window {
private:
  struct setup_state {
    int port = 0;
    bool auto_port = true;
    char bootnodes[65536] = {0};
    bool localhost = false;
    bool auto_nid = true;
    char nid[256] = {0};
  };

  struct control_state {
    node* parent;
    char dl_nid[256] = {0};
    char file[256] = {0};
    std::string popup_nid;
    char last_message[256] = {0};
    std::atomic<backing_store::stats_t> stats;
    std::atomic<bool> keep_stats = true;
    std::thread stats_thread = std::thread{&control_state::stats_body, this};

    void stats_body() {
      while (keep_stats) {
        stats = parent->back()->get_stats();
        std::this_thread::sleep_for(1s);
      }
    }

    control_state(node* parent) : parent{parent} {};
    ~control_state() { keep_stats = false; stats_thread.detach(); }
  };

  class upnp_t {
  private:
    UPNPUrls upnp_urls;
    IGDdatas upnp_data;
    ::UPNPDev* upnp_dev;
    std::string port_str;
    char lan_address[512];

    std::thread renew_loop;
    bool keep_looping = true;
    std::mutex keep_looping_mutex;
    std::condition_variable keep_looping_condvar;

    static constexpr const char* lease_len = "120";
    // Should be shorter, so that we have time to renew
    static constexpr age_t wait_len{90};

    void renew_body() {
      while (keep_looping) {
        auto a = lease_len;

        // Just to be safe
        int res = delete_mapping();

        if (UPNP_AddPortMapping(upnp_urls.controlURL,
                                upnp_data.first.servicetype,
                                port_str.c_str(),
                                port_str.c_str(),
                                lan_address,
                                "c3 Kademlia",
                                "TCP",
                                nullptr, // remote (peer) host address or nullptr for no restriction
                                lease_len) > 0
        )
          throw std::runtime_error("Could not add port mapping");

        std::unique_lock lock{keep_looping_mutex};
        keep_looping_condvar.wait_for(lock, wait_len, [&]() { return !keep_looping; });
      }
    }

    int delete_mapping() {
      return UPNP_DeletePortMapping(upnp_urls.controlURL,
                             upnp_data.first.servicetype,
                             port_str.c_str(),
                             "TCP",
                             nullptr);
    }

  public:
    upnp_t(std::string port) : port_str{port} {
      int error = 0;

      upnp_dev = ::upnpDiscover(1000,    //timeout in milliseconds
                                nullptr, //multicast address, default = "239.255.255.250"
                                nullptr, //minissdpd socket, default = "/var/run/minissdpd.sock"
                                0,       //source port, default = 1900
                                0,       //0 = IPv4, 1 = IPv6
                                10,
                                &error); //error output
      if (upnp_dev == NULL || error != 0)
        throw std::runtime_error("Could not open UPnP");


      UPNPUrls upnp_urls;
      IGDdatas upnp_data;
      if (::UPNP_GetValidIGD(upnp_dev, &upnp_urls, &upnp_data, lan_address, sizeof(lan_address)) != 1) {
        ::FreeUPNPUrls(&upnp_urls);
        ::freeUPNPDevlist(upnp_dev);
        throw std::runtime_error("Could not get Internet Gateway Device");
      }

      renew_loop = std::thread{&upnp_t::renew_body, this};
    }

    ~upnp_t() {
      {
        std::unique_lock lock{keep_looping_mutex};
        keep_looping = false;
        keep_looping_condvar.notify_all();
      }
      if (renew_loop.joinable())
        renew_loop.join();
      delete_mapping();
      ::FreeUPNPUrls(&upnp_urls);
      ::freeUPNPDevlist(upnp_dev);
    }
  };

private:
  bool upnp_on = false;
  std::optional<upnp_t> upnp_data;
  std::thread upnp_update;
  std::optional<node> local;
  std::unique_ptr<setup_state> setup_s;
  std::unique_ptr<control_state> control_s;

public:
  ~window() {

  }

private:
  void do_setup() {
    std::string port_str = std::to_string(setup_s->port);
    std::string addr;
    addr += setup_s->localhost ? "127.0.0.1:" : "0.0.0.0:";
    addr += port_str;

    nid_t nid = setup_s->auto_nid ? generate_nid() : parse_nid(setup_s->nid);

    // TODO: allow selection of type
    std::shared_ptr<backing_store> store = std::make_shared<backing_store::simple>();

    local.emplace(addr, nid, store);

    if (upnp_on)
      upnp_data.emplace(local->get_port());

    std::stringstream bootnodes_stream(setup_s->bootnodes);
    std::string bootnode;

    while (std::getline(bootnodes_stream, bootnode, '\n'))
      try { local->add_peer(bootnode); } catch (...) {}
  }

  void setup() {
    if (!setup_s)
      setup_s = std::make_unique<setup_state>();

    ImGui::Text("Setup:");

    ImGui::Checkbox("UPnP", &upnp_on);
    ImGui::Checkbox("Private node", &setup_s->localhost);

    if (ImGui::Checkbox("Get first free port", &setup_s->auto_port))
      setup_s->port = setup_s->auto_port ? 0 : default_port;

    if (!setup_s->auto_port) {
      if (ImGui::InputInt("Port", &setup_s->port)) {
        if (setup_s->port < 1 || setup_s->port > 65535)
          setup_s->port = default_port;
      }
    }

    if (ImGui::Checkbox("Generate new nid", &setup_s->auto_nid))
      setup_s->port = setup_s->auto_port ? 0 : default_port;

    if (!setup_s->auto_nid) {
      ImGui::InputText("nid", setup_s->nid, sizeof(setup_s->nid));
    }

    ImGui::InputTextMultiline("Bootnodes", setup_s->bootnodes, sizeof(setup_s->bootnodes));

    if (ImGui::Button("Start"))
      do_setup();
  }

  void control() {
    if (!control_s)
      control_s = std::make_unique<control_state>(&*local);
    ImGui::Text("Port: %s", local->get_port().c_str());
    ImGui::Text("Connected nodes: %zu", local->count_peers());
    std::string nid_str = nid_to_string(local->get_nid());
    ImGui::Text("nid: %s", nid_str.c_str());
    if (ImGui::Button("Copy nid to clipboard"))
      ImGui::SetClipboardText(nid_str.c_str());
    ImGui::Text("Storage stats");
    ImGui::Indent( 16.0f );
    ImGui::Columns(2);
    ImGui::Separator();
    {
      // Atomic load
      backing_store::stats_t stats = control_s->stats;
      ImGui::Text("Data stored");
      ImGui::NextColumn();
      ImGui::Text("%zu/%zu bytes", stats.bytes_used, stats.bytes_max);
      ImGui::NextColumn();

      ImGui::Separator();

      ImGui::Text("Values stored");
      ImGui::NextColumn();
      ImGui::Text("%zu/%zu keys", stats.keys_used, stats.keys_max);
      ImGui::NextColumn();
    }
    ImGui::Separator();
    ImGui::Columns(1);
    ImGui::Unindent( 16.0f );
    if (ImGui::Button("Refresh")) {
      try { local->join(); }
      catch (const std::exception& e) {
        snprintf(control_s->last_message, sizeof(control_s->last_message),
                 "%s", e.what());
      }
    }
    if (ImGui::Button("Ping all"))
      local->ping_all();

    ImGui::InputText("File path", control_s->file, sizeof(control_s->file));
    ImGui::InputText("Download nid", control_s->dl_nid, sizeof(control_s->dl_nid));
    if (ImGui::Button("Upload")) {
      try {
        std::ifstream fs(control_s->file, std::ios::binary);
        if (!fs)
          throw std::runtime_error("Could not open file");
        std::vector<uint8_t> buf (
          std::istreambuf_iterator<char>{fs},
          std::istreambuf_iterator<char>{}
        );
        auto stored_nid = local->store(buf);
        snprintf(control_s->last_message, sizeof(control_s->last_message),
                 "Successfully uploaded %s", nid_to_string(stored_nid).c_str());
      }
      catch (const std::exception& e) {
        snprintf(control_s->last_message, sizeof(control_s->last_message),
                 "Failed to upload %s (%s)", control_s->file, e.what());
      }
    }
    if (ImGui::Button("Download")) {
      try {
        auto ret = local->find(parse_nid(control_s->dl_nid));
        if (!ret)
          throw std::runtime_error("Not found");
        std::ofstream fs(control_s->file, std::ios::binary);
        fs.write(reinterpret_cast<const char*>(ret->data()), ret->size());
        snprintf(control_s->last_message, sizeof(control_s->last_message),
                 "Successfully downloaded %s", control_s->dl_nid);
      }
      catch (const std::exception& e) {
        snprintf(control_s->last_message, sizeof(control_s->last_message),
                 "Failed to download %s (%s)", control_s->dl_nid, e.what());
      }
    }

    ImGui::PushItemWidth(ImGui::GetWindowWidth());
    ImGui::InputTextMultiline("", control_s->last_message,
                              sizeof(control_s->last_message),
                              ImVec2(0,0),
                              ImGuiInputTextFlags_ReadOnly);
  }

  void main_loop() {
    ImGui::Text("Node: %s", local ? "Active" : "Inactive");

    if (local)
      control();
    else
      setup();
  }

public:
  window() {
    // Setup SDL
       if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
       {
           printf("Error: %s\n", SDL_GetError());
           throw std::runtime_error("SDL error");
       }

       // Setup window
       SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
       SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
       SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
       SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
       SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
       SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
       SDL_Window* window = SDL_CreateWindow("Dear ImGui SDL2+OpenGL example", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags);
       SDL_GLContext gl_context = SDL_GL_CreateContext(window);
       SDL_GL_MakeCurrent(window, gl_context);
       SDL_GL_SetSwapInterval(1); // Enable vsync

       // Setup Dear ImGui context
       IMGUI_CHECKVERSION();
       ImGui::CreateContext();
       ImGuiIO& io = ImGui::GetIO(); (void)io;
       io.FontGlobalScale = 2.0;
       //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
       //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

       // Setup Dear ImGui style
       ImGui::StyleColorsDark();
       ImGui::GetStyle().WindowRounding = 0.0f;
       ImGui::GetStyle().ChildRounding = 0.0f;
       ImGui::GetStyle().FrameRounding = 0.0f;
       ImGui::GetStyle().GrabRounding = 0.0f;
       ImGui::GetStyle().PopupRounding = 0.0f;
       ImGui::GetStyle().ScrollbarRounding = 0.0f;
       //ImGui::StyleColorsClassic();

       // Setup Platform/Renderer bindings
       ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
       ImGui_ImplOpenGL2_Init();

       // Load Fonts
       // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
       // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
       // - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
       // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
       // - Read 'misc/fonts/README.txt' for more instructions and details.
       // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
       io.Fonts->AddFontDefault();
       //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
       //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
       //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
       //io.Fonts->AddFontFromFileTTF("../../misc/fonts/ProggyTiny.ttf", 10.0f);
       //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
       //IM_ASSERT(font != NULL);

       // Our state
       ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

       // Main loop
       bool done = false;
       while (!done)
       {
           // Poll and handle events (inputs, window resize, etc.)
           // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
           // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application.
           // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application.
           // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
           SDL_Event event;
           while (SDL_PollEvent(&event))
           {
               ImGui_ImplSDL2_ProcessEvent(&event);
               if (event.type == SDL_QUIT)
                   done = true;
           }

           // Start the Dear ImGui frame
           ImGui_ImplOpenGL2_NewFrame();
           ImGui_ImplSDL2_NewFrame(window);
           ImGui::NewFrame();

           int width, height;
           SDL_GetWindowSize(window, &width, &height);
           ImGui::SetNextWindowPos(ImVec2(-1.0f, -1.0f), ImGuiCond_Always);
           ImGui::SetNextWindowSize(ImVec2(width+2, height+2), ImGuiCond_Always);
           bool show_another_window = false;
           ImGui::Begin("GL Editor", &show_another_window, ImGuiWindowFlags_NoResize |
               ImGuiWindowFlags_NoMove |
               ImGuiWindowFlags_NoCollapse |
               ImGuiWindowFlags_NoTitleBar);
           main_loop();
           ImGui::End();

           // Rendering
           ImGui::Render();
           glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
           glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
           glClear(GL_COLOR_BUFFER_BIT);
           //glUseProgram(0); // You may want this if using this code in an OpenGL 3+ context where shaders may be bound
           ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
           SDL_GL_SwapWindow(window);
       }

       // Cleanup
       ImGui_ImplOpenGL2_Shutdown();
       ImGui_ImplSDL2_Shutdown();
       ImGui::DestroyContext();

       SDL_GL_DeleteContext(gl_context);
       SDL_DestroyWindow(window);
       SDL_Quit();
  }
};

int main() {
  window w;

  return 0;
}
