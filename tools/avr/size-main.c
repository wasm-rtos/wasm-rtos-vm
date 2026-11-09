#include "wm.h"
static const uint8_t prog[] = {0x00,0x61,0x73,0x6d,1,0,0,0};
static WmResult led(WmRuntime* r, void* u, uint32_t* s, WmMemory* m){ (void)r;(void)u;(void)m; *(volatile uint8_t*)0x25 = (uint8_t)s[0]; return 0; }
static WmFunc* g_led;
static WmResult resolve(void* c, const char* mo, uint32_t ml, const char* n, uint32_t nl, uint8_t k, WmExtern* o){ o->kind=WM_EXTERN_FUNC; o->as.func=g_led; return 0; }
int main(void){
  static const uint8_t i32[]={WM_I32};
  WmModule* m; WmInstance* in; WmExtern e;
  WmRuntime* rt = wm_runtime_new(256, 16);
  g_led = wm_host_function_new(i32,1,0,0,led,0);
  if (wm_module_load(&m, prog, sizeof prog)) return 1;
  if (wm_instantiate(&in, m, rt, resolve, 0)) return 2;
  if (wm_instance_export(in, "run", 3, &e)) return 3;
  return wm_call(rt, e.as.func, 0, 0) != 0;
}
