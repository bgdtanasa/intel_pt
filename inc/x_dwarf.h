#ifndef _X_DWARF_
#define _X_DWARF_

#define MAX_DWARF_LINE (1024u)
#define MAX_NO_REGS (17u)

extern char* parse_dwarf_exp(const char* const line,
                             const char        terminator,
                             const signed int  terminator_pos,
                             void**            exp);
extern unsigned long long int execute_dwarf_cfa_exp(const void* const      exp,
                                                    unsigned long long int cfa_regs[ MAX_NO_REGS ]);
extern unsigned long long int execute_dwarf_reg_exp(const void* const      exp,
                                                    unsigned long long int cfa_regs[ MAX_NO_REGS ]);

#endif
