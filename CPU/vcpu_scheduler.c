#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <libvirt/libvirt.h>



virDomainStatsRecordPtr * getDomainsInfo(virDomainPtr *doms) {
    virDomainStatsRecordPtr * domstats = NULL;
    if (virDomainListGetStats(doms, VIR_DOMAIN_STATS_VCPU, &domstats, 0) > 0) {
        printf("Cannot get domain stats\n");
    }
    for(virDomainStatsRecordPtr *ptr = domstats; *ptr; ptr++) {
        printf("Domain\n");
        for (int i = 0; i < ((*ptr) -> nparams); i++) {
            printf((*ptr) -> params[i].field);
            printf(": %llu\n", (*ptr) -> params[i].value.ul);
        }
    }
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

unsigned long long int getUsage(virDomainStatsRecordPtr ptr, int vcpu) {
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
    return usage;
}

// Get the usage for each pCPU
void getCpuUsage(virDomainPtr *doms,  virDomainStatsRecordPtr *domstats, int n_doms, int maxcpus, double *cpu_usage) {
    virVcpuInfoPtr cpuinfo;
    unsigned char *cpumaps;
    size_t cpumaplen;
    virDomainStatsRecordPtr *ptr = domstats;
    for(int i = 0; i < n_doms; i++) {
        cpumaplen = VIR_CPU_MAPLEN(maxcpus);
        int vcpu_count = getVcpuCounts(*ptr);
        cpuinfo = calloc(vcpu_count, sizeof(virVcpuInfo));
        cpumaps = calloc(vcpu_count, cpumaplen);
        printf("Domain: %s:\n", virDomainGetName(doms[i]));
        printf("vcpu_count is %d\n", vcpu_count);
        int n_info = virDomainGetVcpus(doms[i], cpuinfo, vcpu_count, cpumaps, cpumaplen);
        printf("return from virDOmainGetVcpus %d\n", n_info);
        if(n_info == -1) {
            continue;
        }
        for (int j = 0; j < vcpu_count; j++) {
            cpu_usage[cpuinfo[j].cpu] += getUsage(*ptr, j);
            printf("CPUmap: 0x%x, ", cpumaps[j]);
            printf("CPU: %d, ", cpuinfo[j].cpu);
            printf("vCPU: %d affinity: ", j);
            for (int m=0; m<maxcpus; m++) {
                printf("%c", VIR_CPU_USABLE(cpumaps, cpumaplen, j, m) ? 'y' : '-' );
            }
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

// Try to average the load for each pcpu
void schedule(virDomainPtr *doms,  virDomainStatsRecordPtr *domstats, int n_doms, double *usage, int maxcpus) {
    double total_load = 0;
    for(int i = 0; i < maxcpus; i++) {
        total_load += usage[i];
    }
    double avg_load = total_load/maxcpus;
    virDomainStatsRecordPtr *ptr = domstats;
    virVcpuInfoPtr cpuinfo;
    unsigned char *cpumaps;
    size_t cpumaplen;
    for(int i = 0; i < n_doms; i++) {
        int vcpu_count = getVcpuCounts(*ptr);
        cpuinfo = calloc(vcpu_count, sizeof(virVcpuInfo));
        cpumaplen = VIR_CPU_MAPLEN(maxcpus);
        cpumaps = calloc(vcpu_count, cpumaplen);
        virDomainGetVcpus(doms[i], cpuinfo, vcpu_count, cpumaps, cpumaplen);
        for (int j = 0; j < vcpu_count; j++) {
            while(usage[cpuinfo[j].cpu] > avg_load) {
                printf("CPU %d is overloaded\n", cpuinfo[j].cpu);
                unsigned long long int load = getUsage(*ptr, j);
                unsigned char available_map = 0x1 << cpuinfo[j].cpu;
                int reload_count = 1;
                for (int k = 0; k < maxcpus; k++) {
                    if (usage[k] < avg_load) {
                        available_map |= (0x1 << k);
                        reload_count += 1;
                    }
                }
                double reload = load / reload_count;
                for (int k = 0; k < maxcpus; k++) {
                    if (usage[k] < avg_load) {
                        usage[k] += reload;
                    }
                }
                usage[cpuinfo[j].cpu] = usage[cpuinfo[j].cpu] - load + reload;
                printf("CPU %d: Usage after rebalance %f\n", cpuinfo[j].cpu, usage[cpuinfo[j].cpu]);
                virDomainPinVcpu(doms[i], j, &available_map, cpumaplen);
            }
        }
        ptr++;
        free(cpuinfo);
        free(cpumaps);

    }
}

void setInitialVcpuPinning(virDomainPtr *doms, virDomainStatsRecordPtr *domstats, int n_domains, int n_cpus) {
    unsigned char map;
    size_t cpumaplen = VIR_CPU_MAPLEN(n_cpus);
    virDomainStatsRecordPtr *ptr = domstats;
    for(int i = 0; i < n_domains; i++) {
        map = 0x1;
        int vcpu_count = getVcpuCounts(*ptr);
        for(int j = 0; j < vcpu_count; j++) {
            printf(" -CPUmap: 0x%x\n", map);
            virDomainPinVcpu(doms[i], j, &map, cpumaplen);
            map <<= 0x1;
            map %= n_cpus;
        }
        ptr ++;
    }
}

/* Main - entry point */
int main(int argc, char **argv) {
    printf("Main function started\n");
    // Connect to hypervisor
    virConnectPtr conn;
    conn = virConnectOpen("qemu:///system");
    int maxcpus = virNodeGetCPUMap(conn, NULL, NULL, 0);
    if (conn == NULL) {
        fprintf(stderr, "Failed to open connection to qemu:///system\n");
        return 1;
    }
    // Get all active running virtual machines
    int numDomains;
    int *activeDomains;
    virDomainPtr *allDomains;

    numDomains = virConnectNumOfDomains(conn);

    activeDomains = malloc(sizeof(int) * numDomains);
    numDomains = virConnectListDomains(conn, activeDomains, numDomains);

    allDomains = malloc(sizeof(virDomainPtr) * numDomains);

    printf("Active domain IDs:\n");
    for (int i = 0 ; i < numDomains ; i++) {
	printf("  %d\n", activeDomains[i]);
        allDomains[i] = virDomainLookupByID(conn, activeDomains[i]);
	int nparams = virDomainGetCPUStats(allDomains[i], NULL, 0, -1, 1, 0);
        
        virTypedParameterPtr params = calloc(nparams, sizeof(virTypedParameter));
        virDomainGetCPUStats(allDomains[i], params, nparams, -1, 1, 0);

        printf("virDomainCPUStats result\n");
        for (int j = 0; j < nparams; j++) {
            printf(params[j].field);
            printf(": %llu, ", params[j].value.ul);
        }
        printf("\n");
    }

    virDomainStatsRecordPtr *domstats = getDomainsInfo(allDomains);
    //setInitialVcpuPinning(allDomains, domstats, numDomains, maxcpus);
    double *pcpu_usage = calloc(maxcpus, sizeof(double));
    getCpuUsage(allDomains, domstats, numDomains, maxcpus, pcpu_usage);
 
  
    schedule(allDomains, domstats, numDomains, pcpu_usage, maxcpus);
    getCpuUsage(allDomains, domstats, numDomains, maxcpus, pcpu_usage);

    //Clean up
    free(activeDomains); 
    virConnectClose(conn);
    free(pcpu_usage);
    //Todo: free allDomains

    return 0;
} 


