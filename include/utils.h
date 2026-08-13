#ifndef utils_h
#define utils_h

#include <sys/types.h>

#include "hwrpb.h"

#ifdef TESTING
#define pal_init()
#else
int		printf (const char *fmt, ...);
struct pcb_struct *find_pa (unsigned long vptb, struct pcb_struct *pcb);
void		pal_init (void);

void *		malloc (size_t size);
void		free (void *ptr);
void		getline (char *buf, int maxlen);
#endif

void		print_memory_map(void);
int		check_memory(unsigned long, unsigned long);
unsigned long	memory_end(void);
extern unsigned long	free_mem_ptr;

#endif /* utils_h */
