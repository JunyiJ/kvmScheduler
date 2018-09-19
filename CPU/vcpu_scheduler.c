#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <libvirt/libvirt.h> 
const long nanosec = 1000000000;
void init_vcpu_usage(double *vcpu_usage, int numDomains, int maxcpus) {
    for(int i = 0; i < numDomains; i++) {
        for (int j = 0; j < maxcpus; j++) {
            *(vcpu_usage + i *numDomains + j) = 0;
        }
    }
}

void print_vcpu_usage(double *vcpu_usage, int numDomains, int maxcpus) {

    for(int i = 0; i < numDomains; i++) {
        printf("VCPU %d :", i);
        for (int j = 0; j < maxcpus; j++) {
            printf("PCPU %d: usage is %f, ", j, *(vcpu_usage + i*numDomains + j));
        }
        printf("\n");
    }
}

virDomainStatsRecordPtr * getDomainsInfo(virDomainPtr *doms) {
    virDomainStatsRecordPtr * domstats = NULL;
    if (virDomainListGetStats(doms, VIR_DOMAIN_STATS_VCPU, &domstats, 0) < 0) {
        printf("Cannot get domain stats\n");
    }
    /*
    for(virDomainStatsRecordPtr *ptr = domstats; *ptr; ptr++) {
        printf("Domain\n");
        for (int i = 0; i < ((*ptr) -> nparams); i++) {
            printf((*ptr) -> params[i].field);
            printf(": %llu\n", (*ptr) -> params[i].value.ul);
        }
    }
    */
    return domstats;
}

int getVcpuCounts(virDomainStatsRecordPtr ptr) {
    int vcpu_count = 0;
    for(int i = 0; i < ptr->nparams; i++) {
        if (strcmp(ptr->params[i].field, "vcpu.current") == 0) {
            vcpu_count = ptr ->params[i].value.i;
            break;
        }
    }
    return vcpu_count;
}

double getUsage(virDomainStatsRecordPtr ptr, int vcpu) {
    unsigned long long int usage = 0;
    for(int i = 0; i < ptr->nparams; i++) {
        int field_len = strlen(ptr->params[i].field);
       if (field_len >= 4) {
           if(strcmp(&(ptr->params[i].field[field_len-4]), "time") == 0) {
               int vcpu_number = atoi(&(ptr->params[i].field[field_len-6]));
               if (vcpu_number == vcpu) {
                   usage += ptr->params[i].value.ul;
                   break;
               }
           }
       }
    }
    return usage*1.0/nanosec;
}

// Get the usage for each pCPU
void getCpuUsage(virDomainPtr *doms,  virDomainStatsRecordPtr *domstats, int n_doms, int maxcpus, 
        double *cpu_usage, double *pre_vcpu_usage, double *cur_vcpu_usage) {
    virVcpuInfoPtr cpuinfo;
    unsigned char *cpumaps;
    size_t cpumaplen;
    virDomainStatsRecordPtr *ptr = domstats;
    //print_vcpu_usage(pre_vcpu_usage, n_doms, maxcpus);

    for(int i = 0; i < n_doms; i++) {
        cpumaplen = VIR_CPU_MAPLEN(maxcpus);
        int vcpu_count = getVcpuCounts(*ptr);
        cpuinfo = calloc(vcpu_count, sizeof(virVcpuInfo));
        cpumaps = calloc(vcpu_count, cpumaplen);
        printf("Domain: %s:\n", virDomainGetName(doms[i]));
        printf("vcpu_count is %d\n", vcpu_count);
        int n_info = virDomainGetVcpus(doms[i], cpuinfo, vcpu_count, cpumaps, cpumaplen);
        if(n_info == -1) {
            continue;
        }
        for (int j = 0; j < vcpu_count; j++) {
            int cur_cpu = cpuinfo[j].cpu;
            *(cur_vcpu_usage + i*n_doms + j) += getUsage(*ptr, j);
            cpu_usage[cur_cpu] += *(cur_vcpu_usage + i*n_doms + j) - *(pre_vcpu_usage + i*n_doms + j);
            printf("CPUmap: 0x%x, ", cpumaps[j]);
            printf("CPU: %d, ", cpuinfo[j].cpu);
            printf("\n");
            free(cpuinfo);
            free(cpumaps);
        }
        ptr++;
    }
    for (int i = 0; i < maxcpus; i++) {
        printf("CPU : %d, Usage: %f\n", i, cpu_usage[i]);
    }
}

//Selection sort for pcpu usage, store the cpu_id in descending order
int * sort_cpu_usage(double *usage, int maxcpus) {
   double *usage_sort = malloc(sizeof(double) * maxcpus);
   int *sorted_cpu_des = malloc(sizeof(int) * maxcpus);
   int *used = malloc(sizeof(int) * maxcpus);
   for (int i = 0; i < maxcpus; i++ ) {
       used[i] = 0;
   }
   for(int i = 0; i < maxcpus; i++) {
       double curmax = -1;
       int maxind = 0;
       for(int j = 0; j < maxcpus; j++) {
           if (!used[j] && usage[j] > curmax) {
               curmax = usage[j];
               maxind = j;
           } else if (!used[j] && usage[j] == curmax) {
               //Using random number to break tie
               if (rand()%maxcpus == 0) {
                   curmax = usage[j];
                   maxind = j;
               }
           }
       }
       usage_sort[i] = curmax;
       sorted_cpu_des[i] = maxind;
       used[maxind] = 1;
   }
   
   free(usage_sort);
   free(used);
   return sorted_cpu_des;
}

int getOrder(int*sorted_cpu_des, int cpu, int maxcpus) {
    for (int i = 0; i < maxcpus; i++) {
        if (sorted_cpu_des[i] == cpu) {
            return i;
        }
    }
    return 0;
}

// Try to average the load for each pcpn
void schedule(virDomainPtr *doms,  virDomainStatsRecordPtr *domstats, int n_doms, double *usage, int maxcpus, double *pre_vcpu_usage, double *cur_vcpu_usage) {
    // Average each vcpu to pcpu at the beginning

    virDomainStatsRecordPtr *ptr = domstats;
    virVcpuInfoPtr cpuinfo;
    unsigned char *cpumaps;
    size_t cpumaplen;
    double total = 0.0, avg = 0.0;
    for(int i = 0; i < n_doms; i++) {
        total += usage[i];
    }
    avg = total / maxcpus;
    printf("avg is %f\n", avg);

    int *sorted_cpu_des = sort_cpu_usage(usage, maxcpus);

    for(int i = 0; i < n_doms; i++) {
        int vcpu_count = getVcpuCounts(*ptr);
        cpuinfo = calloc(vcpu_count, sizeof(virVcpuInfo));
        cpumaplen = VIR_CPU_MAPLEN(maxcpus);
        cpumaps = calloc(vcpu_count, cpumaplen);
        virDomainGetVcpus(doms[i], cpuinfo, vcpu_count, cpumaps, cpumaplen);

        // Swap pCPU usage based on the order.
        // Pair busiest with  freest, 2nd busiest with 2nd freest.
        double variance = 0.4;
        for (int j = 0; j < vcpu_count; j++) {
            int cur_cpu = cpuinfo[j].cpu;
            if (usage[cur_cpu] >= avg + avg * 0.25) { // Allow variance change
                double cur_usage = *(cur_vcpu_usage + i*n_doms + j) - *(pre_vcpu_usage + i*n_doms + j);
                printf("overusge can be lower by %f\n", cur_usage);
                if (usage[cur_cpu] - cur_usage >= avg - avg * variance) {
                    for (int m = maxcpus - 1; m >= 0; m--) {
                        int ind = sorted_cpu_des[m];
                        if (usage[ind] + cur_usage <= avg + avg * variance) {
                            unsigned char swap_map = 0x1 << ind;
                            virDomainPinVcpu(doms[i], j, &swap_map, cpumaplen);

            printf("Swap the vcpu %d from PCPU %d to PCPU %d\n", i, cur_cpu, ind);
                            usage[ind] += cur_usage;

                            usage[cur_cpu] -= cur_usage;
                            break;
                        }
                    }
                }
            }
            sorted_cpu_des = sort_cpu_usage(usage, maxcpus);
            /*
            printf("VCPU %d is using PCPU %d\n", i, cur_cpu);
            int order = getOrder(sorted_cpu_des, cur_cpu, maxcpus); 
            int swap_cpu = sorted_cpu_des[maxcpus - 1 - order];
            for (int p = 0; p < maxcpus; p++ ) {
                printf("%d, ", sorted_cpu_des[p]);
            }
            printf("\n");*/
        }
        ptr++;
        free(cpuinfo);
        free(cpumaps);
    }
    free(sorted_cpu_des);
}

void setInitialVcpuPinning(virDomainPtr *doms, virDomainStatsRecordPtr *domstats, int n_domains, int n_cpus) {
    unsigned char map;
    size_t cpumaplen = VIR_CPU_MAPLEN(n_cpus);
    virDomainStatsRecordPtr *ptr = domstats;
    for(int i = 0; i < n_domains; i++) {
        map = 0x1 << (i % n_cpus);
        int vcpu_count = getVcpuCounts(*ptr);
        for (int j = 0; j < vcpu_count; j++) {
            virDomainPinVcpu(doms[i], j, &map, cpumaplen);
        }
    }
}

/* Main - entry point */
int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Invalid cmd-line args: Use `./vcpu_scheduler interval_time`\n");
        exit(1);
    }
    int interval = atoi(argv[1]);
    // Connect to hypervisor
    virConnectPtr conn;
    conn = virConnectOpen("qemu:///system");
    if (conn == NULL) {
        fprintf(stderr, "Failed to open connection to qemu:///system\n");
        return 1;
    }
    int maxcpus = virNodeGetCPUMap(conn, NULL, NULL, 0);
    double *pcpu_usage = calloc(maxcpus, sizeof(double));
    int numDomains;
    int *activeDomains;
    virDomainPtr *allDomains;

    activeDomains = malloc(sizeof(int) * numDomains);
    allDomains = malloc(sizeof(virDomainPtr) * numDomains);

    numDomains = virConnectNumOfDomains(conn);
    
    numDomains = virConnectListDomains(conn, activeDomains, numDomains);
    // Assume each vcpu only has one process
    double *pre_vcpu_usage = malloc(sizeof(double) * numDomains *maxcpus);
    double *cur_vcpu_usage = malloc(sizeof(double) * numDomains *maxcpus);
    init_vcpu_usage(pre_vcpu_usage, numDomains, maxcpus);
    for(int i = 0; i < numDomains; i++) {
        allDomains[i] = virDomainLookupByID(conn, activeDomains[i]);
    }
    virDomainStatsRecordPtr *domstats = getDomainsInfo(allDomains);
    setInitialVcpuPinning(allDomains, domstats, numDomains, maxcpus);
    while (numDomains) {
        init_vcpu_usage(cur_vcpu_usage, numDomains, maxcpus);
        // Get all active running virtual machines
        numDomains = virConnectNumOfDomains(conn);
        numDomains = virConnectListDomains(conn, activeDomains, numDomains);
        for(int i = 0; i < numDomains; i++) {
            allDomains[i] = virDomainLookupByID(conn, activeDomains[i]);
        }
        virDomainStatsRecordPtr *domstats = getDomainsInfo(allDomains);
        //setInitialVcpuPinning(allDomains, domstats, numDomains, maxcpus);
        for(int i = 0; i < maxcpus; i++) {
            pcpu_usage[i] = 0;
        }
        getCpuUsage(allDomains, domstats, numDomains, maxcpus, pcpu_usage, pre_vcpu_usage, cur_vcpu_usage);
     
        schedule(allDomains, domstats, numDomains, pcpu_usage, maxcpus, pre_vcpu_usage, cur_vcpu_usage);
        for(int i = 0; i < numDomains; i++) {
            for(int j = 0; j < maxcpus; j++) {
                *(pre_vcpu_usage + i*numDomains + j) = *(cur_vcpu_usage + i*numDomains + j);
            }
        }
    //print_vcpu_usage(cur_vcpu_usage, n_doms, maxcpus);
    //init_vcpu_usage(pre_vcpu_usage, n_doms, maxcpus);
    init_vcpu_usage(cur_vcpu_usage, numDomains, maxcpus);

        sleep(interval);
    }
    //Clean up
    free(activeDomains); 
    virConnectClose(conn);
    free(pcpu_usage);
    //Todo: free allDomains

    return 0;
} 


