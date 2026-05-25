#include <stdint.h>

#ifndef IBI3_STAGING_ADDR
#define IBI3_STAGING_ADDR 0x11070000u
#endif

#ifndef INSTALLER_UART_TRACE
#define INSTALLER_UART_TRACE 0
#endif

#ifndef INSTALLER_UART_BAUD
#define INSTALLER_UART_BAUD 9600u
#endif

#define IBI3_MAGIC 0x33494249u
#define IBI3_VERSION 1u
#define IBI3_MAX_RECORDS 16u

#define FLASH_BASE 0x11000000u
#define FLASH_SIZE 0x00080000u
#define FLASH_END (FLASH_BASE + FLASH_SIZE)
#define FLASH_SECTOR_SIZE 0x1000u
#define TARGET_MIN 0x11002000u
#define TARGET_MAX IBI3_STAGING_ADDR

#ifndef INSTALLER_TRACE_ADDR
#define INSTALLER_TRACE_ADDR 0x1107B000u
#endif

#define FLASH_TRACE_ADDR INSTALLER_TRACE_ADDR

#define SYS_CLK_DLL_64M 4u
#define XFRD_FCMD_READ_DUAL 0x0801003bu

#define AP_SPIF_WR_COMPLETION_CTRL (*(volatile uint32_t *)0x4000c838u)
#define AP_SPIF_LOW_WR_PROTECTION  (*(volatile uint32_t *)0x4000c850u)
#define AP_SPIF_UP_WR_PROTECTION   (*(volatile uint32_t *)0x4000c854u)
#define AP_SPIF_WR_PROTECTION      (*(volatile uint32_t *)0x4000c858u)
#define AP_AON_IOCTL0              (*(volatile uint32_t *)0x4000f008u)
#define AP_PCR_SW_RESET0           (*(volatile uint32_t *)0x4000f03cu)
#define AP_PCR_SW_CLK              (*(volatile uint32_t *)0x4000f044u)
#define AP_IOMUX_FULL_MUX0_EN      (*(volatile uint32_t *)0x4000380cu)
#define AP_IOMUX_GPIO_SEL1         (*(volatile uint32_t *)0x4000381cu)
#define AP_UART0_DLL               (*(volatile uint8_t  *)0x40004000u)
#define AP_UART0_DLM               (*(volatile uint8_t  *)0x40004004u)
#define AP_UART0_FCR               (*(volatile uint8_t  *)0x40004008u)
#define AP_UART0_LCR               (*(volatile uint8_t  *)0x4000400cu)
#define AP_UART0_MCR               (*(volatile uint32_t *)0x40004010u)
#define AP_UART0_LSR               (*(volatile uint8_t  *)0x40004014u)
#define AP_UART0_THR               (*(volatile uint8_t  *)0x40004000u)
#define OTA_MODE_SELECT_REG        (*(volatile uint32_t *)0x4000f034u)
#define STATUS_MAGIC_REG           (*(volatile uint32_t *)0x4000f0c8u)
#define STATUS_CODE_REG            (*(volatile uint32_t *)0x4000f0ccu)
#define SCB_AIRCR                  (*(volatile uint32_t *)0xe000ed0cu)

#define BIT(n) (1u << (n))
#define PROGRESS(code) (0x80000000u | (code))
#define LSR_TEMT 0x40u
#define LSR_THRE 0x20u

#if INSTALLER_UART_TRACE
extern uint32_t clk_get_pclk(void);

static uint32_t trace_uart_ready;

static uint32_t trace_uart_div_from_pclk(uint32_t pclk)
{
    return ((pclk >> 4) + (INSTALLER_UART_BAUD >> 1)) / INSTALLER_UART_BAUD;
}

static void trace_uart_apply_div(uint32_t dll)
{
    AP_PCR_SW_CLK |= BIT(7) | BIT(8) | BIT(13);
    AP_PCR_SW_RESET0 &= ~BIT(8);
    AP_PCR_SW_RESET0 |= BIT(8);

    AP_AON_IOCTL0 = (AP_AON_IOCTL0 & ~(3u << 28)) | (2u << 28);
    AP_IOMUX_GPIO_SEL1 = (AP_IOMUX_GPIO_SEL1 & ~(0x3fu << 8)) | (4u << 8);
    AP_IOMUX_FULL_MUX0_EN |= BIT(5);

    AP_UART0_LCR = 0;
    AP_UART0_MCR = 0;
    AP_UART0_LCR = 0x80;
    AP_UART0_DLM = (uint8_t)((dll >> 8) & 0xffu);
    AP_UART0_DLL = (uint8_t)(dll & 0xffu);
    AP_UART0_LCR = 0x03;
    AP_UART0_FCR = 0;

    trace_uart_ready = 1;
}

static void trace_uart_init(void)
{
    if (trace_uart_ready) {
        return;
    }

    trace_uart_apply_div(trace_uart_div_from_pclk(clk_get_pclk()));
}

static void trace_uart_putc(char value)
{
    trace_uart_init();
    while ((AP_UART0_LSR & LSR_THRE) == 0) {
    }
    AP_UART0_THR = (uint8_t)value;
    while ((AP_UART0_LSR & LSR_TEMT) == 0) {
    }
}

static void trace_uart_puts(const char *text)
{
    while (*text) {
        trace_uart_putc(*text++);
    }
}

static void trace_uart_puthex32(uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    int shift;

    for (shift = 28; shift >= 0; shift -= 4) {
        trace_uart_putc(hex[(value >> shift) & 0x0fu]);
    }
}

static unsigned trace_uart_collect_divs(uint32_t *divs, unsigned max_divs)
{
    static const uint32_t fallback_pclks[] = {
        8000000u,
        12000000u,
        16000000u,
        24000000u,
        32000000u,
        48000000u,
        64000000u,
        96000000u,
    };
    unsigned count = 0;
    unsigned index;
    uint32_t runtime_div = trace_uart_div_from_pclk(clk_get_pclk());

    if (runtime_div != 0 && count < max_divs) {
        divs[count++] = runtime_div;
    }

    for (index = 0; index < sizeof(fallback_pclks) / sizeof(fallback_pclks[0]); index++) {
        uint32_t div = trace_uart_div_from_pclk(fallback_pclks[index]);
        unsigned seen = 0;

        while (seen < count) {
            if (divs[seen] == div) {
                break;
            }
            seen++;
        }

        if (seen == count && count < max_divs) {
            divs[count++] = div;
        }
    }

    return count;
}

static void trace_uart_emit_string_all_divs(const char *text)
{
    uint32_t divs[8];
    unsigned count = trace_uart_collect_divs(divs, sizeof(divs) / sizeof(divs[0]));
    unsigned index;

    for (index = 0; index < count; index++) {
        trace_uart_apply_div(divs[index]);
        trace_uart_puts(text);
    }
}

static void trace_uart_emit_hex32_all_divs(char prefix, uint32_t value)
{
    uint32_t divs[8];
    unsigned count = trace_uart_collect_divs(divs, sizeof(divs) / sizeof(divs[0]));
    unsigned index;

    for (index = 0; index < count; index++) {
        trace_uart_apply_div(divs[index]);
        trace_uart_putc(prefix);
        trace_uart_puthex32(value);
        trace_uart_puts("\r\n");
    }
}

void trace_reset_marker(void)
{
    STATUS_MAGIC_REG = IBI3_MAGIC;
    STATUS_CODE_REG = PROGRESS(0x0000u);
    trace_uart_emit_string_all_divs("RST\r\n");
}
#else
void trace_reset_marker(void)
{
}
#endif

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t record_count;
    uint32_t header_size;
    uint32_t image_size;
    uint32_t records_crc16;
    uint32_t flags;
    uint32_t reserved;
} ibi3_header_t;

typedef struct {
    uint32_t target_addr;
    uint32_t source_offset;
    uint32_t size;
    uint32_t crc16;
} ibi3_record_t;

extern int spif_config(uint32_t ref_clk, uint8_t div, uint32_t rd_instr, uint8_t mode_bit, uint8_t qe);
extern int spif_read(uint32_t addr, uint8_t *data, uint32_t size);
extern int spif_write(uint32_t addr, uint8_t *data, uint32_t size);
extern int spif_erase_sector(unsigned int addr);
extern int spif_write_protect(unsigned int enable);

static ibi3_record_t records[IBI3_MAX_RECORDS];
static uint8_t chunk[256];

#if INSTALLER_UART_TRACE
static uint32_t flash_trace_addr;
static uint32_t flash_trace_enabled;

static void flash_trace_append_word(uint32_t value)
{
    if (!flash_trace_enabled || flash_trace_addr >= FLASH_END) {
        return;
    }

    if (spif_write(flash_trace_addr, (uint8_t *)&value, sizeof(value)) != 0) {
        flash_trace_enabled = 0;
        return;
    }

    flash_trace_addr += sizeof(value);
}

static void flash_trace_prepare(uint32_t image_size)
{
    uint32_t magic = IBI3_MAGIC;

    if (flash_trace_enabled) {
        return;
    }

    if (IBI3_STAGING_ADDR + image_size > FLASH_TRACE_ADDR) {
        return;
    }

    if (spif_erase_sector(FLASH_TRACE_ADDR) != 0) {
        return;
    }

    flash_trace_addr = FLASH_TRACE_ADDR;
    flash_trace_enabled = 1;
    flash_trace_append_word(magic);
}
#else
static void flash_trace_append_word(uint32_t value)
{
    (void)value;
}

static void flash_trace_prepare(uint32_t image_size)
{
    (void)image_size;
}
#endif

static void status(uint32_t code)
{
    STATUS_MAGIC_REG = IBI3_MAGIC;
    STATUS_CODE_REG = code;
    flash_trace_append_word(code);
#if INSTALLER_UART_TRACE
    trace_uart_emit_hex32_all_divs('S', code);
#endif
}

static void fail(uint32_t code)
{
    status(code);
    for (;;) {
        __asm volatile("nop");
    }
}

static uint32_t add_overflow(uint32_t a, uint32_t b)
{
    return b > (0xffffffffu - a);
}

static uint32_t align_down(uint32_t value, uint32_t align)
{
    return value & ~(align - 1u);
}

static uint32_t align_up(uint32_t value, uint32_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

static uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint32_t size)
{
    uint32_t i;
    uint32_t bit;

    for (i = 0; i < size; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ 0xa001u);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }

    return crc;
}

static uint16_t crc16_flash(uint32_t addr, uint32_t size)
{
    uint16_t crc = 0;
    uint32_t todo;

    while (size) {
        todo = size;
        if (todo > sizeof(chunk)) {
            todo = sizeof(chunk);
        }
        if (spif_read(addr, chunk, todo) != 0) {
            fail(0x1001u);
        }
        crc = crc16_update(crc, chunk, todo);
        addr += todo;
        size -= todo;
    }

    return crc;
}

static uint32_t record_intersects_sector(const ibi3_record_t *rec, uint32_t sector)
{
    uint32_t rec_end = rec->target_addr + rec->size;
    uint32_t sector_end = sector + FLASH_SECTOR_SIZE;

    return rec->target_addr < sector_end && rec_end > sector;
}

static void validate_record(const ibi3_header_t *hdr, const ibi3_record_t *rec, uint32_t index)
{
    uint32_t source_addr;
    uint32_t source_end;
    uint32_t target_end;
    uint16_t crc;

    if (rec->size == 0) {
        fail(0x2000u | index);
    }
    if (add_overflow(IBI3_STAGING_ADDR, rec->source_offset) ||
        add_overflow(rec->source_offset, rec->size) ||
        add_overflow(rec->target_addr, rec->size)) {
        fail(0x2100u | index);
    }

    source_addr = IBI3_STAGING_ADDR + rec->source_offset;
    source_end = source_addr + rec->size;
    target_end = rec->target_addr + rec->size;

    if (rec->source_offset < hdr->header_size ||
        rec->source_offset + rec->size > hdr->image_size) {
        fail(0x2200u | index);
    }
    if (source_addr < IBI3_STAGING_ADDR || source_end > FLASH_END) {
        fail(0x2300u | index);
    }
    if (rec->target_addr < TARGET_MIN || target_end > TARGET_MAX) {
        fail(0x2400u | index);
    }
    if (rec->target_addr < IBI3_STAGING_ADDR + hdr->image_size &&
        target_end > IBI3_STAGING_ADDR) {
        fail(0x2500u | index);
    }

    crc = crc16_flash(source_addr, rec->size);
    if (crc != (uint16_t)rec->crc16) {
        fail(0x2600u | index);
    }
}

static void erase_targets(const ibi3_header_t *hdr)
{
    uint32_t min_addr = FLASH_END;
    uint32_t max_addr = FLASH_BASE;
    uint32_t sector;
    uint32_t i;
    uint32_t hit;

    for (i = 0; i < hdr->record_count; i++) {
        if (records[i].target_addr < min_addr) {
            min_addr = records[i].target_addr;
        }
        if (records[i].target_addr + records[i].size > max_addr) {
            max_addr = records[i].target_addr + records[i].size;
        }
    }

    min_addr = align_down(min_addr, FLASH_SECTOR_SIZE);
    max_addr = align_up(max_addr, FLASH_SECTOR_SIZE);

    for (sector = min_addr; sector < max_addr; sector += FLASH_SECTOR_SIZE) {
        hit = 0;
        for (i = 0; i < hdr->record_count; i++) {
            if (record_intersects_sector(&records[i], sector)) {
                hit = 1;
                break;
            }
        }
        if (hit) {
            status(PROGRESS(0x0200u | ((sector >> 12) & 0xffu)));
        }
        if (hit && spif_erase_sector(sector) != 0) {
            fail(0x3000u | ((sector >> 12) & 0xffu));
        }
    }
}

static void write_record(const ibi3_record_t *rec, uint32_t index)
{
    uint32_t done = 0;
    uint32_t todo;
    uint32_t page_left;

    while (done < rec->size) {
        todo = rec->size - done;
        if (todo > sizeof(chunk)) {
            todo = sizeof(chunk);
        }
        page_left = 256u - ((rec->target_addr + done) & 0xffu);
        if (todo > page_left) {
            todo = page_left;
        }

        if (spif_read(IBI3_STAGING_ADDR + rec->source_offset + done, chunk, todo) != 0) {
            fail(0x4000u | index);
        }
        if (spif_write(rec->target_addr + done, chunk, todo) != 0) {
            fail(0x4100u | index);
        }

        done += todo;
    }
}

void installer_main(void)
{
    ibi3_header_t hdr;
    uint32_t records_size;
    uint32_t i;
    uint16_t records_crc;

    status(PROGRESS(0x0001u));
    spif_config(SYS_CLK_DLL_64M, 1, XFRD_FCMD_READ_DUAL, 0, 0);
    AP_SPIF_WR_COMPLETION_CTRL = 0xff010005u;
    AP_SPIF_LOW_WR_PROTECTION = 0;
    AP_SPIF_UP_WR_PROTECTION = 0x10u;
    AP_SPIF_WR_PROTECTION = 0x2u;
    spif_write_protect(0);
    status(PROGRESS(0x0010u));

    if (spif_read(IBI3_STAGING_ADDR, (uint8_t *)&hdr, sizeof(hdr)) != 0) {
        fail(0x0100u);
    }
    status(PROGRESS(0x0020u));

    if (hdr.magic != IBI3_MAGIC) {
        fail(0x0101u);
    }
    if (hdr.version != IBI3_VERSION) {
        fail(0x0102u);
    }
    if (hdr.record_count == 0 || hdr.record_count > IBI3_MAX_RECORDS) {
        fail(0x0103u);
    }

    flash_trace_prepare(hdr.image_size);
    flash_trace_append_word(PROGRESS(0x0001u));
    flash_trace_append_word(PROGRESS(0x0010u));
    flash_trace_append_word(PROGRESS(0x0020u));

    records_size = hdr.record_count * sizeof(records[0]);
    if (hdr.header_size < sizeof(hdr) + records_size ||
        hdr.header_size > 0x1000u ||
        hdr.image_size <= hdr.header_size ||
        hdr.image_size > (FLASH_END - IBI3_STAGING_ADDR)) {
        fail(0x0104u);
    }

    if (spif_read(IBI3_STAGING_ADDR + sizeof(hdr), (uint8_t *)records, records_size) != 0) {
        fail(0x0105u);
    }
    records_crc = crc16_update(0, (uint8_t *)records, records_size);
    if (records_crc != (uint16_t)hdr.records_crc16) {
        fail(0x0106u);
    }
    status(PROGRESS(0x0021u));

    for (i = 0; i < hdr.record_count; i++) {
        status(PROGRESS(0x0100u | i));
        validate_record(&hdr, &records[i], i);
    }

    status(PROGRESS(0x0002u));
    erase_targets(&hdr);

    status(PROGRESS(0x0003u));
    for (i = 0; i < hdr.record_count; i++) {
        status(PROGRESS(0x0300u | i));
        write_record(&records[i], i);
    }

    for (i = 0; i < hdr.record_count; i++) {
        status(PROGRESS(0x0400u | i));
        if (crc16_flash(records[i].target_addr, records[i].size) != (uint16_t)records[i].crc16) {
            fail(0x5000u | i);
        }
    }

    status(PROGRESS(0x0004u));
    OTA_MODE_SELECT_REG = 0;
    status(PROGRESS(0x0005u));
    SCB_AIRCR = 0x05fa0004u;
    for (;;) {
        __asm volatile("nop");
    }
}
