#include <stdio.h>
#include <unistd.h>
#include "../common/util.h"

int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    get_cpu_utilization(NULL, NULL);
    while (1) {
        double u, k;
        usleep(500000);
        get_cpu_utilization(&u, &k);
        FILE *fp = fopen(argv[1], "w");
        if (!fp)
            return 2;
        fprintf(fp, "%.2lf %.2lf\n", u, k);
        fclose(fp);
        printf("%.2lf %.2lf\n", u, k);
    }
}
