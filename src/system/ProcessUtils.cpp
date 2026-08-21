#include "ProcessUtils.h"

#include <cstdlib>

namespace ProcessUtils {

bool KillHelloFixProcess() {
    int result = system("taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T");
    return result == 0;
}

}
