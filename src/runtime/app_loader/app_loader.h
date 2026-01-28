#ifndef AKIRA_APP_LOADER_H
#define AKIRA_APP_LOADER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_loader_init(void);
int app_loader_install_memory(const char *name, const void *binary, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_APP_LOADER_H */
