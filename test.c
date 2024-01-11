#include <stdio.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    unsigned long long int value = 6llu;
    for (;;) {
        
        asm volatile ("mov %0, %%rax" : : "r"(value) : "rax");
        asm volatile ("ptwrite %%rax" : : : "rax");
        value++;
        for (unsigned int i = 0u; i < 100000; i++) {

        }
        if ((value % 7llu) == 0llu) {
            value = 0llu;
        }

        //sleep(1);
    }
    return 0;
}