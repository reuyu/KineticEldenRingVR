#include "phys_layer.h"

#include "config.h"
#include "log.h"

namespace kinetic {

namespace {
bool g_enabled = false;
bool g_initialized = false;

void Apply(const char* why) {
    const bool want = ervr::Cfg().physicsEnabled;
    if (g_initialized && want == g_enabled) return;
    g_enabled = want;
    LOGI("[phys] Kinetic physics: %s (%s)", g_enabled ? "on" : "off", why);
    if (g_enabled) LOGI("[phys] M0 skeleton: no physics runs yet");
}
}  // namespace

void PhysInit() {
    Apply("startup");
    g_initialized = true;
}

void PhysConfigChanged() {
    if (!g_initialized) return;
    Apply("ini reload");
}

bool PhysEnabled() { return g_enabled; }

}  // namespace kinetic
