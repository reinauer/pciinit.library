#ifndef PCIINIT_VERSION_H
#define PCIINIT_VERSION_H

/* The Makefile derives these from the nearest v<version>.<revision> tag. */
#if !defined(PCIINIT_VERSION) || !defined(PCIINIT_REVISION) || !defined(PCIINIT_DATE)
#error "pciinit.library takes its version from the Makefile"
#endif
#ifndef PCIINIT_BUILD
#define PCIINIT_BUILD ""
#endif

#define PCIINIT_STRING_(x) #x
#define PCIINIT_STRING(x)  PCIINIT_STRING_(x)
#define PCIINIT_NUMBER     PCIINIT_STRING(PCIINIT_VERSION) "." PCIINIT_STRING(PCIINIT_REVISION)
#define PCIINIT_IDSTRING   "pciinit.library " PCIINIT_NUMBER " (" PCIINIT_DATE ")" PCIINIT_BUILD

#endif
