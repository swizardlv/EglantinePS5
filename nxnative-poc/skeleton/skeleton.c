/*
 * nxnative PoC skeleton title (PPSA98722).
 *
 * Phase 1 lane 1: a minimal ps5link-sdk title that proves, on the console,
 *   1. the VA probe: one sceKernelReserveVirtualRange(0x270000000) (9.75 GiB),
 *      then a 2 MiB commit / touch / decommit round-trip, with a two-segment
 *      fallback (4 GiB + 5.75 GiB) if the single reserve fails;
 *   2. RADV init + one solid-colour frame presented through VK_KHR_display;
 *   3. a heap time series for the Phase 0 design reconciliation
 *      (guest-heap commit cap 6.0 GiB, target range 5.0-6.0 GiB).
 *
 * Logging rule (user 2026-11-07 08:13): every check-point logs, so one console
 * run is enough to locate a failure without asking the user to re-run or
 * describe what they saw. All logs go to klog AND to files inside the title's
 * own directory (/app0 = PPSA98722/ on the USB when run from USB):
 *   /app0/nxnative-poc.txt      - full human-readable run log
 *   /app0/nxnative-poc-va.txt   - VA probe verdict + one line per reserve call
 *   /app0/nxnative-poc-heap.csv - heap ticks (startup / reserve / commit /
 *                                 decommit / RADV init / every 30 frames / exit)
 *
 * What this title does NOT do: no game code, no NUP reading, no network.
 * Nothing here claims a console result until it has run on the console:
 * every verdict below is produced at run time from real return codes.
 *
 * Build: nxnative-poc/skeleton/build-skeleton.sh (inside the
 * eglantineps5-builder container; reuses the RADV archive + link recipe
 * from /data/code/mihawk-vulkan-review, read-only).
 *
 * Derived from the proven patterns in mihawk-99/PS5_Vulkan radv_smoke.c
 * (direct vk_icdGetInstanceProcAddr entry, VK_KHR_display present loop,
 * klog + /app0 results file) and in EglantinePS5 src/memory_pages.cpp
 * (ReserveAddressRange / CommitMemoryRangeFrom / DecommitMemoryRange) and
 * src/direct_memory.c (eden_direct_high: allocate from the top of the pool).
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------- title facts */

#define NXNATIVE_TITLE_ID "PPSA98722"
/* TITLE_ID is a confirmed value (user 2026-11-07 08:10). Never PPSA98515. */
#define NXNATIVE_RESULTS_PATH "/app0/nxnative-poc.txt"
#define NXNATIVE_VA_PATH "/app0/nxnative-poc-va.txt"
#define NXNATIVE_HEAP_PATH "/app0/nxnative-poc-heap.csv"
#define NXNATIVE_ENV_PATH "/app0/nxnative-poc-env.txt"

/* VA probe: phase0-3 section 4. mem_reserve 0x270000000 = 9.75 GiB. */
#define NXNATIVE_MEM_RESERVE ((size_t)0x270000000ull)
#define NXNATIVE_SPLIT_FIRST ((size_t)0x100000000ull)  /* 4 GiB */
#define NXNATIVE_SPLIT_SECOND ((size_t)0x170000000ull) /* 5.75 GiB */
#define NXNATIVE_LARGE_PAGE ((size_t)0x200000ull)      /* 2 MiB */
#define NXNATIVE_CPU_MAP_LO ((uintptr_t)0x300000000ull)
#define NXNATIVE_CPU_MAP_HI ((uintptr_t)0x4000000000ull)
#define NXNATIVE_HINT ((void *)0x1000000000ull)
/* Guest-heap commit cap, phase0-3 section 1.4: 6.0 GiB. The skeleton commits
 * only one 2 MiB page; the cap is the reconciliation target, not a claim. */
#define NXNATIVE_HEAP_CAP_BYTES ((size_t)0x180000000ull)

/* Solid colour presented: deep blue, distinct from the smoke test's grays. */
#define NXNATIVE_CLEAR_R 0.05f
#define NXNATIVE_CLEAR_G 0.35f
#define NXNATIVE_CLEAR_B 0.85f
#define NXNATIVE_FRAMES 120 /* ~2 s at 60 Hz FIFO: long enough to see. */
#define NXNATIVE_HEAP_EVERY_FRAMES 30

/* ------------------------------------------------------------ kernel decls */

/* No official header declares these (Eglantine declares them extern "C"
 * itself in src/memory_pages.cpp); the signatures below are copied from
 * there, not invented. */
extern int sceKernelDebugOutText(int channel, const char *text);
extern int ps5_klog_capture_stderr(const char *prefix);
extern int64_t sceKernelGetDirectMemorySize(void);
extern int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end,
                                             size_t size, size_t alignment, int type,
                                             int64_t *physical);
extern int32_t sceKernelMapDirectMemory(void **address, size_t size, int protection,
                                        int flags, int64_t physical, size_t alignment);
extern int32_t sceKernelReleaseDirectMemory(int64_t physical, size_t size);
extern int32_t sceKernelReserveVirtualRange(void **address, size_t size, int flags,
                                            size_t alignment);

/* ------------------------------------------------------------------ logging */

static FILE *results;
static FILE *va_log;
static FILE *heap_log;
static unsigned passed, failed;

static void
report(const char *format, ...)
{
   char line[768];
   va_list args;
   va_start(args, format);
   vsnprintf(line, sizeof(line), format, args);
   va_end(args);
   fprintf(stderr, "[nxnative-poc] %s\n", line);
   fflush(stderr);
   char klog_line[832];
   snprintf(klog_line, sizeof(klog_line), "[nxnative-poc] %s\n", line);
   sceKernelDebugOutText(0, klog_line);
   if (results) {
      fprintf(results, "%s\n", line);
      fflush(results);
   }
}

static void
check(bool ok, const char *what)
{
   report("%s %s", ok ? "PASS" : "FAIL", what);
   if (ok)
      passed++;
   else
      failed++;
}

/* ---------------------------------------------------------------- heap log */

/* There is NO kernel query for free direct memory or for the largest free
 * block: the SDK exposes only sceKernelGetDirectMemorySize() (the pool
 * total). Eglantine's guard likewise measures its own tracked commit, not
 * system free. So this log records what is knowable per tick, and brackets
 * the largest single allocatable block with a bounded ladder probe (allocate
 * N, release at once; the first success is a lower bound, the smallest
 * failure an upper bound). free_bytes is logged as "unknown" on purpose --
 * inventing it would be worse than admitting it. */
static int64_t heap_pool_total = -1;     /* sceKernelGetDirectMemorySize() */
static size_t heap_guest_committed;      /* bytes this title committed */
static size_t heap_guest_reserved;       /* VA only, not physical */
static int64_t heap_radv_heap;           /* VkPhysicalDeviceMemoryProperties heap[0] */
static unsigned heap_tick_count;

static int64_t
heap_probe_largest_free(size_t *lower_bound, size_t *upper_bound)
{
   /* Ladder, largest first. Each attempt allocates then releases at once,
    * so nothing is held. 6 GiB is the guest-heap cap: if a single 6 GiB
    * block lands, the cap is placeable in one block. */
   static const size_t ladder[] = {(size_t)0x180000000ull, (size_t)0xC0000000ull,
                                   (size_t)0x40000000ull, (size_t)0x10000000ull,
                                   (size_t)0x200000ull};
   size_t lb = 0, ub = (size_t)heap_pool_total; /* no failure: bounded by the pool */
   bool any_success = false;
   for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
      int64_t physical = -1;
      int32_t rc = sceKernelAllocateDirectMemory(0, heap_pool_total, ladder[i],
                                                 NXNATIVE_LARGE_PAGE, 12, &physical);
      if (rc == 0 && physical >= 0) {
         (void)sceKernelReleaseDirectMemory(physical, ladder[i]);
         lb = ladder[i];
         any_success = true;
         break; /* first success is the largest ladder step that lands */
      }
      ub = ladder[i]; /* smallest size that failed so far */
   }
   (void)any_success;
   *lower_bound = lb;
   *upper_bound = ub;
   return 0;
}

static void
heap_tick(const char *phase)
{
   heap_tick_count++;
   heap_pool_total = sceKernelGetDirectMemorySize();
   size_t lb = 0, ub = 0;
   heap_probe_largest_free(&lb, &ub);
   char line[512];
   snprintf(line, sizeof(line),
            "%u,%s,%lld,%zu,%zu,%lld,%zu,%zu,unknown",
            heap_tick_count, phase, (long long)heap_pool_total, heap_guest_reserved,
            heap_guest_committed, (long long)heap_radv_heap, lb, ub);
   if (heap_log) {
      fprintf(heap_log, "%s\n", line);
      fflush(heap_log);
   }
   report("HEAP tick=%u phase=%s pool_mib=%lld reserved_mib=%zu committed_bytes=%zu "
          "radv_heap_mib=%lld largest_free_lb_mib=%zu largest_free_ub_mib=%zu "
          "free_bytes=unknown(no kernel query)",
          heap_tick_count, phase, (long long)(heap_pool_total / (1024 * 1024)),
          heap_guest_reserved / (1024 * 1024), heap_guest_committed,
          (long long)(heap_radv_heap < 0 ? -1 : heap_radv_heap / (1024 * 1024)),
          lb / (1024 * 1024), ub / (1024 * 1024));
}

/* ---------------------------------------------------------------- VA probe */

/* One reserve call, logged to the VA file and the main log. Returns the base
 * or NULL. The kernel keeps the hint only as a hint: where the range really
 * lands is reported, and the probe checks it against phase0-3 section 4.4:
 * [0x300000000, 0x40_0000_0000), 2 MiB aligned. */
static void *
va_reserve_once(size_t size, const char *label, int32_t *rc_out)
{
   void *address = NXNATIVE_HINT;
   int32_t rc = sceKernelReserveVirtualRange(&address, size, 0, NXNATIVE_LARGE_PAGE);
   *rc_out = rc;
   bool in_range = false;
   if (rc == 0 && address != NULL && address != MAP_FAILED) {
      uintptr_t start = (uintptr_t)address;
      in_range = start >= NXNATIVE_CPU_MAP_LO && start % NXNATIVE_LARGE_PAGE == 0 &&
                 start <= NXNATIVE_CPU_MAP_HI - size;
   }
   if (va_log) {
      fprintf(va_log, "reserve label=%s size_mib=%zu rc=%d base=%p in_range=%d\n",
              label, size / (1024 * 1024), (int)rc, address, in_range ? 1 : 0);
      fflush(va_log);
   }
   report("VA reserve label=%s size_mib=%zu rc=%d base=%p in_range=%d", label,
          size / (1024 * 1024), (int)rc, address, in_range ? 1 : 0);
   if (rc != 0 || !in_range)
      return NULL;
   return address;
}

/* Commit one 2 MiB page at `at`, from the TOP of the pool (phase0-3 section
 * 4.4: long-held blocks go through eden_direct_high, pool top down, so the
 * pool does not fragment from the bottom). Mirrors Eglantine
 * src/direct_memory.c eden_direct_high: 2 MiB steps down from the pool total,
 * bounded tries, then a whole-pool fallback. The probe's page is transient
 * (committed, touched, decommitted in the same run); the production
 * guest-heap commit path will use the same top-down rule for held blocks. */
static int32_t
va_commit_top_down(size_t size, int64_t *physical_out)
{
   int64_t total = sceKernelGetDirectMemorySize();
   int64_t bytes = (int64_t)size;
   int32_t rc = -1;
   int64_t next = total;
   for (unsigned tries = 0; tries < 512 && next >= bytes; tries++) {
      int64_t window = (next - bytes) / (int64_t)NXNATIVE_LARGE_PAGE * (int64_t)NXNATIVE_LARGE_PAGE;
      next = window;
      rc = sceKernelAllocateDirectMemory(window, window + bytes, size,
                                         NXNATIVE_LARGE_PAGE, 12, physical_out);
      if (rc == 0)
         return 0;
   }
   rc = sceKernelAllocateDirectMemory(0, total, size, NXNATIVE_LARGE_PAGE, 12, physical_out);
   return rc;
}

struct va_result {
   bool single_ok;      /* one 9.75 GiB reserve landed in range */
   bool split_ok;       /* fallback: both segments landed in range */
   void *base;          /* reserve kept until process exit */
   size_t size;         /* size of the kept reserve */
   void *commit_base;   /* page committed / touched / decommitted */
   bool commit_ok;
   bool decommit_ok;
};

static struct va_result
va_probe(void)
{
   struct va_result r = {0};
   heap_tick("startup");
   if (va_log) {
      fprintf(va_log, "# nxnative-poc VA probe, title " NXNATIVE_TITLE_ID "\n");
      fprintf(va_log, "# single reserve: size_mib=9984 (0x270000000, 9.75 GiB)\n");
      fprintf(va_log, "# fallback split: 4096 MiB + 5888 MiB (4 GiB + 5.75 GiB)\n");
      fflush(va_log);
   }
   report("VA probe starts: single reserve 9984 MiB (0x270000000)");

   int32_t rc = 0;
   void *base = va_reserve_once(NXNATIVE_MEM_RESERVE, "single-9.75GiB", &rc);
   if (base) {
      r.single_ok = true;
      r.base = base;
      r.size = NXNATIVE_MEM_RESERVE;
      r.commit_base = base;
   } else {
      /* Single reserve failed (or landed out of range): release what the
       * failed call may have left, then try the documented fallback. */
      report("VA single reserve failed; falling back to split reserve");
      if (va_log) {
         fprintf(va_log, "fallback reason: single rc=%d\n", (int)rc);
         fflush(va_log);
      }
      void *first = va_reserve_once(NXNATIVE_SPLIT_FIRST, "split-4GiB", &rc);
      int32_t rc2 = 0;
      void *second = va_reserve_once(NXNATIVE_SPLIT_SECOND, "split-5.75GiB", &rc2);
      if (first && second) {
         r.split_ok = true;
         r.base = first; /* segment 1 is the commit-test segment */
         r.size = NXNATIVE_SPLIT_FIRST;
         r.commit_base = first;
         heap_guest_reserved = NXNATIVE_SPLIT_FIRST + NXNATIVE_SPLIT_SECOND;
         report("VA split reserve: BOTH segments landed; guest heap would be "
                "managed as two segments (design branch, see report)");
      } else {
         if (first)
            (void)munmap(first, NXNATIVE_SPLIT_FIRST);
         if (second)
            (void)munmap(second, NXNATIVE_SPLIT_SECOND);
         report("VA split reserve FAILED; no guest-heap reservation available");
         if (va_log) {
            fprintf(va_log, "verdict: FAIL single_and_split\n");
            fflush(va_log);
         }
         heap_tick("va-failed");
         return r;
      }
   }
   if (r.single_ok)
      heap_guest_reserved = NXNATIVE_MEM_RESERVE;
   heap_tick("va-reserved");

   /* Commit one 2 MiB page at the base, touch it, give it back. */
   void *at = r.commit_base;
   int64_t physical = -1;
   int32_t commit_rc = va_commit_top_down(NXNATIVE_LARGE_PAGE, &physical);
   report("VA commit size_mib=2 top_down rc=%d physical=0x%llx", (int)commit_rc,
          (unsigned long long)physical);
   if (va_log) {
      fprintf(va_log, "commit size_mib=2 top_down rc=%d physical=0x%llx\n",
              (int)commit_rc, (unsigned long long)physical);
      fflush(va_log);
   }
   if (commit_rc == 0 && physical >= 0) {
      void *mapped = at;
      int32_t map_rc = sceKernelMapDirectMemory(&mapped, NXNATIVE_LARGE_PAGE,
                                                PROT_READ | PROT_WRITE, MAP_FIXED,
                                                physical, NXNATIVE_LARGE_PAGE);
      report("VA map rc=%d mapped=%p expected=%p", (int)map_rc, mapped, at);
      if (map_rc == 0 && mapped == at) {
         volatile uint8_t *p = (volatile uint8_t *)at;
         p[0] = 0x5A;
         p[NXNATIVE_LARGE_PAGE - 1] = 0xA5;
         bool touched = p[0] == 0x5A && p[NXNATIVE_LARGE_PAGE - 1] == 0xA5;
         report("VA touch first=0x%02x last=0x%02x ok=%d", p[0],
                p[NXNATIVE_LARGE_PAGE - 1], touched ? 1 : 0);
         heap_guest_committed = NXNATIVE_LARGE_PAGE;
         heap_tick("va-committed");
         /* Decommit: unmap, re-reserve the address space at once, then
          * release the physical page (memory_pages.cpp DecommitMemoryRange). */
         int munmap_rc = munmap(at, NXNATIVE_LARGE_PAGE);
         void *again = at;
         int32_t re_rc = sceKernelReserveVirtualRange(&again, NXNATIVE_LARGE_PAGE,
                                                      MAP_FIXED, NXNATIVE_LARGE_PAGE);
         int32_t rel_rc = sceKernelReleaseDirectMemory(physical, NXNATIVE_LARGE_PAGE);
         r.commit_ok = touched;
         r.decommit_ok = munmap_rc == 0 && re_rc == 0 && again == at && rel_rc == 0;
         report("VA decommit munmap_rc=%d re_reserve_rc=%d again=%p release_rc=%d "
                "ok=%d", munmap_rc, (int)re_rc, again, (int)rel_rc,
                r.decommit_ok ? 1 : 0);
         if (va_log) {
            fprintf(va_log,
                    "decommit munmap_rc=%d re_reserve_rc=%d release_rc=%d ok=%d\n",
                    munmap_rc, (int)re_rc, (int)rel_rc, r.decommit_ok ? 1 : 0);
            fflush(va_log);
         }
         heap_guest_committed = 0;
         heap_tick("va-decommitted");
      } else {
         (void)sceKernelReleaseDirectMemory(physical, NXNATIVE_LARGE_PAGE);
         report("VA map FAILED; physical page released, reservation kept");
      }
   }

   if (va_log) {
      fprintf(va_log, "verdict: %s single=%d split=%d commit=%d decommit=%d "
                      "reserved_base=%p reserved_mib=%zu\n",
              (r.single_ok || r.split_ok) && r.commit_ok && r.decommit_ok ? "PASS" : "FAIL",
              r.single_ok ? 1 : 0, r.split_ok ? 1 : 0, r.commit_ok ? 1 : 0,
              r.decommit_ok ? 1 : 0, r.base, r.size / (1024 * 1024));
      fflush(va_log);
   }
   check((r.single_ok || r.split_ok) && r.commit_ok && r.decommit_ok,
         "VA probe: reserve + 2 MiB commit/touch/decommit round-trip");
   report("VA probe ends: single=%d split=%d commit=%d decommit=%d reserved_base=%p",
          r.single_ok ? 1 : 0, r.split_ok ? 1 : 0, r.commit_ok ? 1 : 0,
          r.decommit_ok ? 1 : 0, r.base);
   /* The reservation is deliberately kept until process exit: the later
    * guest heap will own this address range. Nothing is unmapped here. */
   return r;
}

/* ------------------------------------------------------------ RADV context */

/* No Vulkan loader on PS5: every command comes from RADV's
 * vk_icdGetInstanceProcAddr (radv probe report section 2). The lists below
 * are the smoke test's, trimmed to what a clear + present needs. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                   const char *name);

#define INSTANCE_COMMANDS(X)                                                        \
   X(DestroyInstance)                                                               \
   X(EnumeratePhysicalDevices)                                                      \
   X(GetPhysicalDeviceProperties2)                                                  \
   X(GetPhysicalDeviceQueueFamilyProperties)                                        \
   X(GetPhysicalDeviceMemoryProperties)                                             \
   X(GetPhysicalDeviceFeatures)                                                     \
   X(GetPhysicalDeviceFeatures2)                                                    \
   X(CreateDevice)                                                                  \
   X(EnumerateDeviceExtensionProperties)                                            \
   X(GetDeviceProcAddr)

#define DISPLAY_INSTANCE_COMMANDS(X)                                               \
   X(GetPhysicalDeviceDisplayPropertiesKHR)                                         \
   X(GetDisplayModePropertiesKHR)                                                   \
   X(CreateDisplayPlaneSurfaceKHR)                                                  \
   X(DestroySurfaceKHR)                                                             \
   X(GetPhysicalDeviceSurfaceSupportKHR)                                            \
   X(GetPhysicalDeviceSurfaceCapabilitiesKHR)                                       \
   X(GetPhysicalDeviceSurfaceFormatsKHR)

#define DEVICE_COMMANDS(X)                                                          \
   X(DestroyDevice)                                                                 \
   X(GetDeviceQueue)                                                                \
   X(QueueSubmit)                                                                   \
   X(QueueWaitIdle)                                                                 \
   X(DeviceWaitIdle)                                                                \
   X(CreateFence)                                                                   \
   X(DestroyFence)                                                                  \
   X(WaitForFences)                                                                 \
   X(ResetFences)                                                                   \
   X(CreateCommandPool)                                                             \
   X(DestroyCommandPool)                                                            \
   X(AllocateCommandBuffers)                                                        \
   X(BeginCommandBuffer)                                                            \
   X(EndCommandBuffer)                                                              \
   X(ResetCommandBuffer)                                                            \
   X(CreateSemaphore)                                                               \
   X(DestroySemaphore)                                                              \
   X(CreateSwapchainKHR)                                                            \
   X(DestroySwapchainKHR)                                                           \
   X(GetSwapchainImagesKHR)                                                         \
   X(AcquireNextImageKHR)                                                           \
   X(QueuePresentKHR)                                                               \
   X(CmdClearColorImage)                                                            \
   X(CmdPipelineBarrier2)

#define DECLARE(name) static PFN_vk##name vk##name;
INSTANCE_COMMANDS(DECLARE)
DISPLAY_INSTANCE_COMMANDS(DECLARE)
DEVICE_COMMANDS(DECLARE)
static PFN_vkCreateInstance vkCreateInstance;
#undef DECLARE

static bool
load_instance(VkInstance instance)
{
   bool ok = true;
#define LOAD(name)                                                                \
   vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(instance, "vk" #name);      \
   if (!vk##name) {                                                               \
      report("RADV load: missing vk" #name);                                      \
      ok = false;                                                                 \
   }
   INSTANCE_COMMANDS(LOAD)
   DISPLAY_INSTANCE_COMMANDS(LOAD)
#undef LOAD
   report("RADV load: instance commands %s", ok ? "all present" : "MISSING entries (see above)");
   return ok;
}

static bool
load_device(VkDevice device)
{
   bool ok = true;
#define LOAD(name)                                                                \
   vk##name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name);              \
   if (!vk##name) {                                                               \
      report("RADV load: missing vk" #name);                                      \
      ok = false;                                                                 \
   }
   DEVICE_COMMANDS(LOAD)
#undef LOAD
   report("RADV load: device commands %s", ok ? "all present" : "MISSING entries (see above)");
   return ok;
}

struct context {
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice device;
   VkQueue queue;
   uint32_t family;
   VkPhysicalDeviceMemoryProperties memory;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
   bool display;
   bool mesh;
   bool task;
};

static void
log_extension(const char *name, bool present)
{
   report("RADV ext: %s %s", name, present ? "reported" : "NOT reported");
}

static bool
context_create(struct context *c)
{
   vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE,
                                                                     "vkCreateInstance");
   report("RADV step: vk_icdGetInstanceProcAddr(vkCreateInstance) %s",
          vkCreateInstance ? "found" : "NULL");
   if (!vkCreateInstance) {
      check(false, "RADV step: vk_icdGetInstanceProcAddr has vkCreateInstance");
      return false;
   }
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nxnative-poc",
      .apiVersion = VK_API_VERSION_1_4,
   };
   const PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions =
      (PFN_vkEnumerateInstanceExtensionProperties)vk_icdGetInstanceProcAddr(
          VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
   VkExtensionProperties instance_extensions[64];
   uint32_t instance_extension_count = sizeof(instance_extensions) / sizeof(instance_extensions[0]);
   c->display = false;
   VkResult enum_rc = VK_ERROR_INITIALIZATION_FAILED;
   if (enumerate_instance_extensions)
      enum_rc = enumerate_instance_extensions(NULL, &instance_extension_count,
                                              instance_extensions);
   report("RADV step: vkEnumerateInstanceExtensionProperties rc=%d count=%u",
          (int)enum_rc, instance_extension_count);
   if (enum_rc >= VK_SUCCESS) {
      for (uint32_t i = 0; i < instance_extension_count; i++)
         c->display |= strcmp(instance_extensions[i].extensionName,
                              VK_KHR_DISPLAY_EXTENSION_NAME) == 0;
   }
   report("RADV step: VK_KHR_display %s", c->display ? "reported" : "NOT reported");
   const char *const display_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                             VK_KHR_DISPLAY_EXTENSION_NAME};
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = c->display ? 2 : 0,
      .ppEnabledExtensionNames = display_extensions,
   };
   VkResult result = vkCreateInstance(&instance_info, NULL, &c->instance);
   report("RADV step: vkCreateInstance rc=%d", (int)result);
   check(result == VK_SUCCESS, "RADV step: vkCreateInstance");
   if (result != VK_SUCCESS || !load_instance(c->instance))
      return false;

   uint32_t count = 1;
   result = vkEnumeratePhysicalDevices(c->instance, &count, &c->physical);
   report("RADV step: vkEnumeratePhysicalDevices rc=%d count=%u", (int)result, count);
   check((result == VK_SUCCESS || result == VK_INCOMPLETE) && count == 1,
         "RADV step: one physical device");
   if (count == 0)
      return false;

   VkPhysicalDeviceDriverProperties driver = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
   VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                        .pNext = &driver};
   vkGetPhysicalDeviceProperties2(c->physical, &props);
   report("RADV device: %s, Vulkan %u.%u.%u, %s %s", props.properties.deviceName,
          VK_API_VERSION_MAJOR(props.properties.apiVersion),
          VK_API_VERSION_MINOR(props.properties.apiVersion),
          VK_API_VERSION_PATCH(props.properties.apiVersion),
          driver.driverName, driver.driverInfo);

   VkQueueFamilyProperties families[8];
   count = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(c->physical, &count, families);
   c->family = UINT32_MAX;
   for (uint32_t i = 0; i < count; i++) {
      if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
          (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
         c->family = i;
         break;
      }
   }
   report("RADV step: queue families=%u graphics+compute family=%s", count,
          c->family == UINT32_MAX ? "NONE" : "found");
   if (c->family == UINT32_MAX) {
      check(false, "RADV step: a graphics and compute queue family");
      return false;
   }
   vkGetPhysicalDeviceMemoryProperties(c->physical, &c->memory);
   for (uint32_t i = 0; i < c->memory.memoryHeapCount; i++)
      report("RADV heap: heap[%u] size_mib=%llu flags=0x%x", i,
             (unsigned long long)(c->memory.memoryHeaps[i].size / (1024 * 1024)),
             c->memory.memoryHeaps[i].flags);
   if (c->memory.memoryHeapCount > 0)
      heap_radv_heap = (int64_t)c->memory.memoryHeaps[0].size;

   /* Device extensions: log the Phase 0 / RADV-probe checklist one by one,
    * so a missing entry is named in the log instead of hiding in a total. */
   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = c->family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkPhysicalDeviceVulkan12Features features12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .drawIndirectCount = VK_TRUE,
   };
   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &features12,
      .synchronization2 = VK_TRUE,
      .dynamicRendering = VK_TRUE,
   };
   const char *device_extensions[12];
   uint32_t enabled_extensions = 0;
   VkExtensionProperties extensions[512];
   uint32_t extension_count = sizeof(extensions) / sizeof(extensions[0]);
   bool mesh = false, task = false, barycentric = false, shading_rate = false;
   bool acceleration_structure = false, descriptor_buffer = false;
   bool timeline = false, buffer_device_address = false, descriptor_indexing = false;
   bool cooperative_matrix = false;
   if (vkEnumerateDeviceExtensionProperties(c->physical, NULL, &extension_count,
                                           extensions) >= VK_SUCCESS) {
      report("RADV step: vkEnumerateDeviceExtensionProperties count=%u",
             extension_count);
      for (uint32_t i = 0; i < extension_count; i++) {
         const char *n = extensions[i].extensionName;
         mesh |= strcmp(n, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0;
         barycentric |= strcmp(n, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0;
         shading_rate |= strcmp(n, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0;
         acceleration_structure |=
             strcmp(n, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0;
         descriptor_buffer |= strcmp(n, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) == 0;
         timeline |= strcmp(n, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0;
         buffer_device_address |=
             strcmp(n, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0;
         descriptor_indexing |= strcmp(n, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) == 0;
         cooperative_matrix |=
             strcmp(n, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) == 0;
      }
   } else {
      report("RADV step: vkEnumerateDeviceExtensionProperties FAILED");
   }
   log_extension(VK_EXT_MESH_SHADER_EXTENSION_NAME, mesh);
   log_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, acceleration_structure);
   log_extension(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, descriptor_buffer);
   log_extension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, timeline);
   log_extension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, buffer_device_address);
   log_extension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, descriptor_indexing);
   log_extension(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, shading_rate);
   log_extension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, cooperative_matrix);
   report("RADV note: VRS/cooperative-matrix are EXPECTED absent on GFX10.3 "
          "(RADV probe report); mesh needs RADV_PS5_MESH=1 (set at startup)");
   c->mesh = mesh;
   c->task = false;
   VkPhysicalDeviceMeshShaderFeaturesEXT mesh_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
   };
   if (mesh) {
      VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                         .pNext = &mesh_features};
      vkGetPhysicalDeviceFeatures2(c->physical, &query);
      c->mesh = mesh_features.meshShader;
      task = mesh_features.taskShader;
      c->task = task;
      report("RADV mesh: meshShader=%d taskShader=%d (features query)",
             c->mesh ? 1 : 0, task ? 1 : 0);
      mesh_features.pNext = NULL;
      mesh_features.multiviewMeshShader = VK_FALSE;
      mesh_features.primitiveFragmentShadingRateMeshShader = VK_FALSE;
      mesh_features.meshShaderQueries = VK_FALSE;
      device_extensions[enabled_extensions++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
      mesh_features.pNext = features13.pNext;
      features13.pNext = &mesh_features;
   } else {
      report("RADV mesh: VK_EXT_mesh_shader NOT reported even with "
             "RADV_PS5_MESH=1; clear-only path does not need it");
   }
   if (acceleration_structure) {
      device_extensions[enabled_extensions++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
      device_extensions[enabled_extensions++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
      features12.bufferDeviceAddress = VK_TRUE;
   }
   if (barycentric) {
      /* Declared at function scope: features13.pNext still points at it
       * when vkCreateDevice reads the chain below. */
      static VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary;
      bary.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR;
      bary.pNext = NULL;
      bary.fragmentShaderBarycentric = VK_TRUE;
      device_extensions[enabled_extensions++] =
          VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME;
      bary.pNext = features13.pNext;
      features13.pNext = &bary;
   }
   if (c->display)
      device_extensions[enabled_extensions++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
   VkPhysicalDeviceFeatures supported;
   vkGetPhysicalDeviceFeatures(c->physical, &supported);
   const VkPhysicalDeviceFeatures features = {
      .sparseBinding = supported.sparseBinding,
      .geometryShader = VK_TRUE,
      .tessellationShader = VK_TRUE,
      .vertexPipelineStoresAndAtomics = VK_TRUE,
      .fragmentStoresAndAtomics = VK_TRUE,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features13,
      .pEnabledFeatures = &features,
      .enabledExtensionCount = enabled_extensions,
      .ppEnabledExtensionNames = device_extensions,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   result = vkCreateDevice(c->physical, &device_info, NULL, &c->device);
   report("RADV step: vkCreateDevice rc=%d enabled_extensions=%u", (int)result,
          enabled_extensions);
   check(result == VK_SUCCESS, "RADV step: vkCreateDevice");
   if (result != VK_SUCCESS || !load_device(c->device))
      return false;
   vkGetDeviceQueue(c->device, c->family, 0, &c->queue);
   report("RADV step: vkGetDeviceQueue queue=%p", (void *)c->queue);

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = c->family,
   };
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   result = vkCreateCommandPool(c->device, &pool_info, NULL, &c->pool);
   report("RADV step: vkCreateCommandPool rc=%d", (int)result);
   if (result != VK_SUCCESS)
      return false;
   const VkCommandBufferAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = c->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   result = vkAllocateCommandBuffers(c->device, &alloc_info, &c->cmd);
   report("RADV step: vkAllocateCommandBuffers rc=%d", (int)result);
   if (result != VK_SUCCESS)
      return false;
   result = vkCreateFence(c->device, &fence_info, NULL, &c->fence);
   report("RADV step: vkCreateFence rc=%d", (int)result);
   if (result != VK_SUCCESS)
      return false;
   heap_tick("radv-init");
   check(true, "RADV init: instance + device + queue + command pool + fence");
   return true;
}

/* ----------------------------------------------------------------- display */

static void
display_image_barrier(struct context *c, VkImage image, VkImageLayout from,
                      VkImageLayout to, VkPipelineStageFlags2 src_stage,
                      VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                      VkAccessFlags2 dst_access)
{
   const VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
   };
   vkCmdPipelineBarrier2(c->cmd, &dependency);
}

/* One frame: acquire, clear to the solid colour, present. Every step logs
 * its VkResult, and the FIRST frame's results are logged individually so a
 * failure is attributable to acquire / submit / present, not to a total. */
static VkResult
display_frame(struct context *c, VkSwapchainKHR swapchain, const VkImage *images,
              VkSemaphore acquired, VkSemaphore rendered, unsigned frame)
{
   uint32_t index = 0;
   VkResult result = vkAcquireNextImageKHR(c->device, swapchain, UINT64_MAX, acquired,
                                           VK_NULL_HANDLE, &index);
   if (frame == 0)
      report("RADV frame 0: vkAcquireNextImageKHR rc=%d index=%u", (int)result, index);
   if (result != VK_SUCCESS)
      return result;
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   vkResetCommandBuffer(c->cmd, 0);
   vkBeginCommandBuffer(c->cmd, &begin_info);
   display_image_barrier(c, images[index], VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
   const VkClearColorValue color = {.float32 = {NXNATIVE_CLEAR_R, NXNATIVE_CLEAR_G,
                                                NXNATIVE_CLEAR_B, 1.0f}};
   const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   vkCmdClearColorImage(c->cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &color, 1, &range);
   display_image_barrier(c, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
   vkEndCommandBuffer(c->cmd);

   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &acquired,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &c->cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &rendered,
   };
   vkResetFences(c->device, 1, &c->fence);
   result = vkQueueSubmit(c->queue, 1, &submit, c->fence);
   if (frame == 0)
      report("RADV frame 0: vkQueueSubmit rc=%d", (int)result);
   if (result != VK_SUCCESS)
      return result;
   const VkPresentInfoKHR present = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &rendered,
      .swapchainCount = 1,
      .pSwapchains = &swapchain,
      .pImageIndices = &index,
   };
   result = vkQueuePresentKHR(c->queue, &present);
   if (frame == 0)
      report("RADV frame 0: vkQueuePresentKHR rc=%d (first frame submitted)", (int)result);
   vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX);
   return result;
}

static bool
present_solid_colour(struct context *c)
{
   VkDisplayPropertiesKHR display;
   uint32_t count = 1;
   VkResult result = vkGetPhysicalDeviceDisplayPropertiesKHR(c->physical, &count, &display);
   report("RADV display: vkGetPhysicalDeviceDisplayPropertiesKHR rc=%d count=%u",
          (int)result, count);
   if (result != VK_SUCCESS || count != 1) {
      check(false, "RADV display: one display");
      return false;
   }
   report("RADV display: %s %ux%u", display.displayName, display.physicalResolution.width,
          display.physicalResolution.height);
   VkDisplayModePropertiesKHR modes[2];
   count = 2;
   result = vkGetDisplayModePropertiesKHR(c->physical, display.display, &count, modes);
   report("RADV display: vkGetDisplayModePropertiesKHR rc=%d modes=%u first_refresh_hz=%.3f",
          (int)result, count,
          count > 0 ? modes[0].parameters.refreshRate / 1000.0 : 0.0);
   /* VK_INCOMPLETE means the fixed modes[2] array was filled and more modes
      exist; modes[0] is valid, so accept it like the physical-device
      enumerate above (line ~560). */
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
      check(false, "RADV display: its modes");
      return false;
   }
   const VkDisplaySurfaceCreateInfoKHR surface_info = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = modes[0].displayMode,
      .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .imageExtent = {display.physicalResolution.width, display.physicalResolution.height},
   };
   VkSurfaceKHR surface = VK_NULL_HANDLE;
   result = vkCreateDisplayPlaneSurfaceKHR(c->instance, &surface_info, NULL, &surface);
   report("RADV display: vkCreateDisplayPlaneSurfaceKHR rc=%d", (int)result);
   check(result == VK_SUCCESS, "RADV display: vkCreateDisplayPlaneSurfaceKHR");
   if (result != VK_SUCCESS)
      return false;
   VkBool32 supported = VK_FALSE;
   vkGetPhysicalDeviceSurfaceSupportKHR(c->physical, c->family, surface, &supported);
   VkSurfaceCapabilitiesKHR caps;
   vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->physical, surface, &caps);
   report("RADV display: surface supported=%d currentExtent=%ux%u minImages=%u "
          "maxImages=%u",
          supported ? 1 : 0, caps.currentExtent.width, caps.currentExtent.height,
          caps.minImageCount, caps.maxImageCount);
   if (!supported) {
      check(false, "RADV display: surface supported by the queue family");
      vkDestroySurfaceKHR(c->instance, surface, NULL);
      return false;
   }

   VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
   const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkResult sem_rc1 = vkCreateSemaphore(c->device, &semaphore_info, NULL, &acquired);
   VkResult sem_rc2 = vkCreateSemaphore(c->device, &semaphore_info, NULL, &rendered);
   report("RADV display: semaphores rc=%d/%d", (int)sem_rc1, (int)sem_rc2);
   if (sem_rc1 != VK_SUCCESS || sem_rc2 != VK_SUCCESS) {
      check(false, "RADV display: its semaphores");
      vkDestroySurfaceKHR(c->instance, surface, NULL);
      return false;
   }

   const VkSwapchainCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = 3,
      .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = {display.physicalResolution.width, display.physicalResolution.height},
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
      .oldSwapchain = VK_NULL_HANDLE,
   };
   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   result = vkCreateSwapchainKHR(c->device, &info, NULL, &swapchain);
   report("RADV display: vkCreateSwapchainKHR rc=%d extent=%ux%u", (int)result,
          info.imageExtent.width, info.imageExtent.height);
   check(result == VK_SUCCESS, "RADV display: vkCreateSwapchainKHR");
   if (result != VK_SUCCESS) {
      vkDestroySemaphore(c->device, acquired, NULL);
      vkDestroySemaphore(c->device, rendered, NULL);
      vkDestroySurfaceKHR(c->instance, surface, NULL);
      return false;
   }
   VkImage images[8];
   count = 8;
   result = vkGetSwapchainImagesKHR(c->device, swapchain, &count, images);
   report("RADV display: vkGetSwapchainImagesKHR rc=%d count=%u", (int)result, count);
   if (result != VK_SUCCESS || count < 3) {
      check(false, "RADV display: 3+ swapchain images");
      vkDestroySwapchainKHR(c->device, swapchain, NULL);
      vkDestroySemaphore(c->device, acquired, NULL);
      vkDestroySemaphore(c->device, rendered, NULL);
      vkDestroySurfaceKHR(c->instance, surface, NULL);
      return false;
   }

   unsigned presented = 0;
   VkResult first_failure = VK_SUCCESS;
   unsigned first_failure_frame = 0;
   for (unsigned frame = 0; frame < NXNATIVE_FRAMES; frame++) {
      result = display_frame(c, swapchain, images, acquired, rendered, frame);
      if (result != VK_SUCCESS) {
         if (first_failure == VK_SUCCESS) {
            first_failure = result;
            first_failure_frame = frame;
         }
         report("RADV frame %u: FAILED rc=%d (first failure)", frame, (int)result);
         break;
      }
      presented++;
      if ((frame + 1) % NXNATIVE_HEAP_EVERY_FRAMES == 0) {
         report("RADV frame %u/%u presented", frame + 1, NXNATIVE_FRAMES);
         heap_tick("present");
      }
   }
   report("RADV present: %u/%u frames presented, first_failure_frame=%u rc=%d",
          presented, NXNATIVE_FRAMES, first_failure_frame, (int)first_failure);
   check(presented == NXNATIVE_FRAMES,
         "RADV present: all solid-colour frames presented");
   VkResult idle_rc = vkDeviceWaitIdle(c->device);
   report("RADV display: vkDeviceWaitIdle rc=%d", (int)idle_rc);
   vkDestroySwapchainKHR(c->device, swapchain, NULL);
   vkDestroySemaphore(c->device, acquired, NULL);
   vkDestroySemaphore(c->device, rendered, NULL);
   vkDestroySurfaceKHR(c->instance, surface, NULL);
   return presented == NXNATIVE_FRAMES;
}

/* -------------------------------------------------------------------- main */

/* Exit codes: 0 = VA probe PASS + solid colour presented; 1 = probe logged
 * but display/present failed (the logs still name the step); 2 = RADV init
 * failed (probe results are already in the log files). */
int
main(void)
{
   const int captured = ps5_klog_capture_stderr("[nxnative-poc:stderr] ");
   /* Optional NAME=VALUE lines for the driver's environment (RADV_DEBUG,
    * ACO_DEBUG), read before the driver is. Same mechanism as radv_smoke. */
   FILE *const env = fopen(NXNATIVE_ENV_PATH, "r");
   if (env) {
      char line[256];
      while (fgets(line, sizeof(line), env)) {
         line[strcspn(line, "\r\n")] = '\0';
         char *const equals = strchr(line, '=');
         if (equals && line[0] != '#') {
            *equals = '\0';
            setenv(line, equals + 1, 1);
         }
      }
      fclose(env);
   }
   /* RADV_PS5_MESH defaults to off in the driver; the mesh/task path is a
    * Phase 1 check-point, so enable it unless the env file said otherwise. */
   if (!getenv("RADV_PS5_MESH"))
      setenv("RADV_PS5_MESH", "1", 1);
   results = fopen(NXNATIVE_RESULTS_PATH, "w");
   va_log = fopen(NXNATIVE_VA_PATH, "w");
   heap_log = fopen(NXNATIVE_HEAP_PATH, "w");
   if (heap_log) {
      fprintf(heap_log,
              "tick,phase,pool_total_bytes,guest_reserved_bytes,guest_committed_bytes,"
              "radv_heap_bytes,largest_free_lb_bytes,largest_free_ub_bytes,free_bytes\n");
      fflush(heap_log);
   }
   heap_pool_total = sceKernelGetDirectMemorySize();
   heap_radv_heap = -1;
   report("nxnative-poc starts, title " NXNATIVE_TITLE_ID
          " (RADV_PS5_MESH=%s, stderr-to-klog=%s)",
          getenv("RADV_PS5_MESH") ? getenv("RADV_PS5_MESH") : "?",
          captured == 0 ? "captured" : "not captured");
   fprintf(stderr, "standard error reaches klog\n");

   /* Check-point 1: VA probe (with its own va file). */
   struct va_result va = va_probe();
   bool va_ok = (va.single_ok || va.split_ok) && va.commit_ok && va.decommit_ok;

   /* Check-point 2+3: RADV init, then the solid-colour present. */
   struct context c = {0};
   bool radv_ok = context_create(&c);
   bool shown = false;
   if (radv_ok) {
      if (c.display)
         shown = present_solid_colour(&c);
      else
         report("RADV display: VK_KHR_display NOT reported; present skipped, "
                "init alone is not a pass");
   }
   heap_tick("exit");
   if (c.device) {
      vkDeviceWaitIdle(c.device);
      vkDestroyFence(c.device, c.fence, NULL);
      vkDestroyCommandPool(c.device, c.pool, NULL);
      vkDestroyDevice(c.device, NULL);
   }
   if (c.instance)
      vkDestroyInstance(c.instance, NULL);
   report("nxnative-poc ends: %u passed, %u failed; VA %s, RADV init %s, "
          "solid colour %s; logs: " NXNATIVE_RESULTS_PATH ", " NXNATIVE_VA_PATH
          ", " NXNATIVE_HEAP_PATH,
          passed, failed, va_ok ? "PASS" : "FAIL", radv_ok ? "PASS" : "FAIL",
          shown ? "PRESENTED" : "NOT SHOWN");
   if (results)
      fclose(results);
   if (va_log)
      fclose(va_log);
   if (heap_log)
      fclose(heap_log);
   if (!radv_ok)
      return 2;
   if (!shown)
      return 1;
   return va_ok ? 0 : 1;
}
