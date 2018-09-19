#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <libvirt/libvirt.h>


struct DomMem {
    virDomainPtr dom;
    unsigned long long int mem;
};

void sort_usage(struct DomMem *dom_mem, int n_dom) {
   struct DomMem *dom_mem_tmp = malloc(sizeof(struct DomMem) * n_dom);
   int used[n_dom];
   for(int i = 0; i < n_dom; i++) {
       struct DomMem curmax;
       curmax.dom = NULL;
       curmax.mem = 0;
       int ind = 0;
       for(int j = 0; j < n_dom; j++) {
           if(!used[j] && dom_mem[j].mem >= curmax.mem) {
               curmax.dom = dom_mem[j].dom;
               curmax.mem = dom_mem[j].mem;
               ind = j;
           }
       }
       dom_mem_tmp[i].dom = curmax.dom;
       dom_mem_tmp[i].mem = curmax.mem;
       used[ind] = 1;
   }
   
   for(int i = 0; i < n_dom; i++) {
       dom_mem[i].dom = dom_mem_tmp[i].dom;
       dom_mem[i].mem = dom_mem_tmp[i].mem;
   }
   free(dom_mem_tmp);
}

void coordinate(virDomainPtr *doms, int n_dom) {
    struct DomMem *dom_mem = malloc(sizeof(struct DomMem) * n_dom);
    for(int i = 0; i < n_dom; i++) {
        virDomainMemoryStatStruct dom_mem_stat[VIR_DOMAIN_MEMORY_STAT_NR];
        if(virDomainSetMemoryStatsPeriod(doms[i], 1, VIR_DOMAIN_AFFECT_CURRENT) < 0) {
            printf("Error: Cannot enable memory ballon driver statistics\n");
        }
        if(virDomainMemoryStats(doms[i], dom_mem_stat, VIR_DOMAIN_MEMORY_STAT_NR, 0) < 0) {
            printf("Error: Cannot get memory stats\n");
        }
        printf("Domain %d has %llu Memory available\n", i, dom_mem_stat[VIR_DOMAIN_MEMORY_STAT_AVAILABLE].val);
        dom_mem[i].dom = doms[i];
        dom_mem[i].mem = dom_mem_stat[VIR_DOMAIN_MEMORY_STAT_AVAILABLE].val;
    }
    unsigned long long int available_lower_bound = 50 * 1024;
    unsigned long long int available_upper_bound = 300 * 1024;
    for (int i = 0; i < n_dom; i++){
        if (dom_mem[i].mem < available_lower_bound) {
            for(int j = 0; j < n_dom; j++) {
                if (dom_mem[j].mem >= available_upper_bound) {
                    printf("Transfer memory from wasteful domain %s to starve domain %s\n",
                            virDomainGetName(dom_mem[j].dom), virDomainGetName(dom_mem[i].dom));
                    unsigned long long int amount = dom_mem[j].mem - available_upper_bound;
                    dom_mem[j].mem = available_lower_bound;
                    virDomainSetMemory(dom_mem[j].dom, dom_mem[j].mem - amount);
                    virDomainSetMemory(dom_mem[i].dom, dom_mem[j].mem + amount);
                    dom_mem[i].mem += amount;
                    if (dom_mem[i].mem >= available_lower_bound) break;
                }
            }
            if (dom_mem[i].mem < available_lower_bound) {
                printf("Mem is %llu, Assign memory from hypervisor to starve domain\n", dom_mem[i].mem);
                virDomainSetMemory(dom_mem[i].dom, dom_mem[i].mem + available_lower_bound);
            }
        } else {
            if (dom_mem[i].mem >= available_upper_bound) {
                printf("Give memory back to hypervisor from domain %s\n", virDomainGetName(dom_mem[i].dom));
                virDomainSetMemory(dom_mem[i].dom, dom_mem[i].mem - available_upper_bound);
            }
        }
    } 
    free(dom_mem);
}

/* Main - entry point */
int main(int argc, char **argv) {
    if(argc != 2) {
        printf("Error: Need one cmdline argument: time interval in seconds\n");
        return 1;
    }
    int interval = atoi(argv[1]);
    virConnectPtr conn;
    conn = virConnectOpen("qemu:///system");
    if (conn == NULL) {
        printf("Error: Failed to open connection to qemu:///system\n");
        return 1;
    }
    int numDomains;
    int *activeDomains;
    virDomainPtr *allDomains;
    activeDomains = malloc(sizeof(int) * numDomains);
    allDomains = malloc(sizeof(virDomainPtr) * numDomains);
    numDomains = virConnectNumOfDomains(conn);
    
    while(numDomains > 0) {
        numDomains = virConnectListDomains(conn, activeDomains, numDomains);
        for(int i = 0; i < numDomains; i++) {
            allDomains[i] = virDomainLookupByID(conn, activeDomains[i]);
        }
        coordinate(allDomains, numDomains);
        sleep(interval);
    }

    free(allDomains);
    free(activeDomains);
    virConnectClose(conn);
    return 0;
}

