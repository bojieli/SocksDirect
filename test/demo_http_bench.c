#include <stdio.h>
#include <unistd.h>
#include "../common/util.h"

int main() {
    get_cpu_utilization(NULL, NULL);
    while (1) {
        double u, k;
        sleep(1);
        get_cpu_utilization(&u, &k);
        printf("user=%.2lf, kernel=%.2lf\n", u, k);
    }
}
