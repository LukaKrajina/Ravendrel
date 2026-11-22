#include "test_framework.hpp"
#include "Kokkos_Core.hpp"

int main() {
    int rc = qtest::run_all();
    if (Kokkos::is_initialized())
        Kokkos::finalize();
    return rc;
}