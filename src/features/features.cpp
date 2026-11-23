#include "features/features.h"

#include "core/hook.h"
#include "core/log.h"
#include "features/dlss.h"
#include "features/fg/fg.h"
#include "features/nav.h"
#include "features/stereo/stereo.h"

namespace th {

void features_register() {
  dlss_register();
  fg_register();
  nav_register();
  stereo_register();
}

void features_start() {
  hook_batch_begin();
  dlss_start();
  fg_start();
  nav_start();
  stereo_start();
  hook_batch_end();
}

}  // namespace th
