#include "private/pciinit.h"
#include "private/quirks.h"
#include <proto/exec.h>
#include <proto/dos.h>

uint32_t pi_options(const struct TagItem *tags, struct PIOptions *opts)
{
    unsigned budget = 256;
    *opts           = (struct PIOptions){0};
    opts->quirks    = 1;
    while (tags && budget--) {
        uint32_t tag = tags->ti_Tag, data = tags->ti_Data;
        ++tags;
        switch (tag) {
        case TAG_DONE:
            return PI_OK;
        case TAG_IGNORE:
            break;
        case TAG_MORE:
            tags = (const struct TagItem *)data;
            break;
        case TAG_SKIP:
            if (data > budget)
                return PI_BAD_ARGUMENT;
            budget -= data;
            tags += data;
            break;
        case PII_Error:
            opts->error = (uint32_t *)data;
            break;
        case PII_Offline:
            opts->offline = data;
            break;
        case PII_Diagnostics:
            opts->diagnostics = (struct PCIInitDiagnostics *)data;
            break;
        case PII_Hardware:
            opts->hardware = data;
            break;
        case PII_Limits:
            opts->limits = (const struct X86Limits *)data;
            break;
        case PII_Quirks:
            opts->quirks = data;
            break;
        case PII_AccessHook:
            opts->hook = (struct Hook *)data;
            break;
        case PII_AccessRanges:
            opts->ranges = (const struct PCIInitAccessRange *)data;
            break;
        case PII_ServiceLog:
            opts->services = (struct PCIInitServiceLog *)data;
            break;
        case PII_StopOnBusError:
            opts->stop_bus_error = data;
            break;
        case PII_ReleaseVGA:
            opts->release_vga = data;
            break;
        default:
            return PI_BAD_ARGUMENT;
        }
    }
    return tags ? PI_BAD_ARGUMENT : PI_OK;
}
int pi_board_exists(struct Library *pci, struct pci_dev *board)
{
    struct pci_dev *p = NULL;
    if (!board)
        return 0;
    while ((p = op_find(pci, p)))
        if (p == board)
            return 1;
    return 0;
}
static PCIInitImage *finish(struct PIBase *base, uint8_t *data, uint32_t bytes, uint32_t *status)
{
    PCIInitImage *image = pi_alloc(sizeof(*image));
    if (!image) {
        *status = PI_NO_MEMORY;
        pi_free(data);
        return NULL;
    }
    image->info.size = sizeof(image->info);
    *status          = pi_rom_parse(data, bytes, &image->info);
    if (*status) {
        pi_free(image);
        pi_free(data);
        return NULL;
    }
    image->crc         = pi_crc32(data + image->info.image_offset, image->info.init_bytes);
    image->info.source = data;
    image->owner       = base;
    image->data        = data;
    ++base->objects;
    return image;
}
PCIInitImage *pi_load_file(struct PIBase *base, const char *path, const struct TagItem *tags)
{
    struct ExecBase *SysBase   = base->sys;
    struct DosLibrary *DOSBase = NULL;
    struct PIOptions opts;
    PCIInitImage *image = NULL;
    uint32_t status     = pi_options(tags, &opts);
    uint8_t *data       = NULL;
    BPTR file           = 0;
    LONG bytes;
    if (status)
        goto out;
    if (!path || opts.offline || opts.limits || opts.hardware || opts.diagnostics) {
        status = PI_BAD_ARGUMENT;
        goto out;
    }
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR) "dos.library", 39);
    if (!DOSBase) {
        status = PI_IO_ERROR;
        goto out;
    }
    file = Open((STRPTR)path, MODE_OLDFILE);
    if (!file || Seek(file, 0, OFFSET_END) < 0) {
        status = PI_IO_ERROR;
        goto out;
    }
    bytes = Seek(file, 0, OFFSET_BEGINNING);
    if (bytes < 512 || bytes > (LONG)PCIINIT_MAX_ROM) {
        status = PI_BAD_ROM;
        goto out;
    }
    data = pi_alloc(bytes);
    if (!data) {
        status = PI_NO_MEMORY;
        goto out;
    }
    if (Read(file, data, bytes) != bytes) {
        status = PI_IO_ERROR;
        goto out;
    }
    image = finish(base, data, bytes, &status);
    data  = NULL;
out:
    pi_free(data);
    if (file)
        Close(file);
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);
    if (opts.error)
        *opts.error = status;
    return image;
}
PCIInitImage *pi_load_card(struct PIBase *base, struct pci_dev *board, const struct TagItem *tags)
{
    struct ExecBase *SysBase = base->sys;
    struct Library *pci      = NULL;
    struct PIOptions opts;
    PCIInitImage *image = NULL;
    uint32_t status     = pi_options(tags, &opts), address, bytes, i, reg, command, rom;
    uint8_t *data       = NULL;
    unsigned owned      = 0, header;
    if (status)
        goto out;
    if (base->mapping_owner) {
        status = PI_BAD_STATE;
        goto out;
    }
    if (!board || opts.offline || opts.limits || opts.hardware || opts.diagnostics) {
        status = PI_BAD_ARGUMENT;
        goto out;
    }
    pci = op_open(SysBase);
    if (!pci) {
        status = PI_NO_OPENPCI;
        goto out;
    }
    if (!pi_board_exists(pci, board)) {
        status = PI_BAD_ARGUMENT;
        goto out;
    }
    if (!op_obtain(pci, board)) {
        status = PI_CARD_BUSY;
        goto out;
    }
    owned  = 1;
    header = op_read(pci, board, PCI_HEADER_TYPE, 1) & 0x7f;
    if (header > 1) {
        status = PI_UNSUPPORTED_ROM;
        goto out;
    }
    reg     = header ? 0x38 : PCI_ROM_ADDRESS;
    address = op_attr(pci, board, PRM_ROM_Address);
    bytes   = op_attr(pci, board, PRM_ROM_Size);
    if (!address || address == 0xffffffffUL || bytes < 512 || bytes > PCIINIT_MAX_ROM ||
        address > 0xffffffffUL - bytes) {
        status = PI_NO_ROM;
        goto out;
    }
    data = pi_alloc(bytes);
    if (!data) {
        status = PI_NO_MEMORY;
        goto out;
    }
    command = op_read(pci, board, PCI_COMMAND, 2);
    rom     = op_read(pci, board, reg, 4);
    op_write(pci, board, PCI_COMMAND, command | PCI_COMMAND_MEMORY, 2);
    op_write(pci, board, reg, rom | 1, 4);
    for (i = 0; i < bytes; ++i)
        data[i] = ((volatile uint8_t *)address)[i];
    op_write(pci, board, reg, rom, 4);
    op_write(pci, board, PCI_COMMAND, command, 2);
    if (op_read(pci, board, reg, 4) != rom || op_read(pci, board, PCI_COMMAND, 2) != command) {
        status = PI_IO_ERROR;
        goto out;
    }
    /* No parser or DOS calls while temporary hardware state is installed. */
    op_release(pci, board);
    owned = 0;
    /* An assigned ROM window without the ROM signature holds no ROM: a
     * bridge or a card without firmware reads as FFh or nothing. */
    if (data[0] != 0x55 || data[1] != 0xaa) {
        status = PI_NO_ROM;
        goto out;
    }
    image = finish(base, data, bytes, &status);
    data  = NULL;
out:
    pi_free(data);
    if (owned)
        op_release(pci, board);
    if (pci)
        CloseLibrary(pci);
    if (opts.error)
        *opts.error = status;
    return image;
}
uint32_t pi_free_image(struct PIBase *base, PCIInitImage *image)
{
    if (!image || image->owner != base)
        return PI_BAD_ARGUMENT;
    pi_free(image->data);
    pi_free(image);
    --base->objects;
    return PI_OK;
}
