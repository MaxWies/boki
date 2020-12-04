#include "utils/random.h"

#include <sys/syscall.h>
#include <random>

namespace faas {
namespace utils {

namespace {
static thread_local std::mt19937_64 rd_gen(syscall(SYS_gettid));
}

int GetRandomInt(int a, int b) {
    DCHECK_LT(a, b);
    std::uniform_int_distribution<int> distribution(a, b - 1);
    return distribution(rd_gen);
}

float GetRandomFloat(float a, float b) {
    DCHECK_LE(a, b);
    std::uniform_real_distribution<float> distribution(a, b);
    return distribution(rd_gen);
}

double GetRandomDouble(double a, double b) {
    DCHECK_LE(a, b);
    std::uniform_real_distribution<double> distribution(a, b);
    return distribution(rd_gen);
}

}  // namespace utils
}  // namespace faas
