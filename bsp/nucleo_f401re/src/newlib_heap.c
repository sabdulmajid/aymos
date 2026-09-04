#include <errno.h>
#include <stddef.h>

void *_sbrk(ptrdiff_t increment);
void _init(void);
void _fini(void);

void *_sbrk(ptrdiff_t increment)
{
    (void)increment;
    errno = ENOMEM;
    return (void *)-1;
}

void _init(void)
{
}

void _fini(void)
{
}
