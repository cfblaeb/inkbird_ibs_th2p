
/home/laeb/Downloads/inkbird_ibs_th2p/inkbird_fw/build/stock_bundle_installer/STOCK_BUNDLE_INSTALLER.elf:     file format elf32-littlearm


Disassembly of section .text:

1fff1838 <_vectors>:
1fff1838:	1fffd000 	.word	0x1fffd000
1fff183c:	1fff1841 	.word	0x1fff1841

1fff1840 <Reset_Handler>:

.section .text.Reset_Handler, "ax", %progbits
.global Reset_Handler
.type Reset_Handler, %function
Reset_Handler:
    cpsid i
1fff1840:	b672      	cpsid	i
    ldr r0, =_stack_top
1fff1842:	480c      	ldr	r0, [pc, #48]	@ (1fff1874 <Reset_Handler+0x34>)
    msr msp, r0
1fff1844:	f380 8808 	msr	MSP, r0
    movs r0, #0
1fff1848:	2000      	movs	r0, #0
    msr psp, r0
1fff184a:	f380 8809 	msr	PSP, r0
    msr control, r0
1fff184e:	f380 8814 	msr	CONTROL, r0
    isb
1fff1852:	f3bf 8f6f 	isb	sy

    ldr r0, =_sbss
1fff1856:	4808      	ldr	r0, [pc, #32]	@ (1fff1878 <Reset_Handler+0x38>)
    ldr r1, =_ebss
1fff1858:	4908      	ldr	r1, [pc, #32]	@ (1fff187c <Reset_Handler+0x3c>)
    movs r2, #0
1fff185a:	2200      	movs	r2, #0
1:
    cmp r0, r1
1fff185c:	4288      	cmp	r0, r1
    bcc 2f
1fff185e:	d300      	bcc.n	1fff1862 <Reset_Handler+0x22>
    b 3f
1fff1860:	e002      	b.n	1fff1868 <Reset_Handler+0x28>
2:
    str r2, [r0]
1fff1862:	6002      	str	r2, [r0, #0]
    adds r0, r0, #4
1fff1864:	3004      	adds	r0, #4
    b 1b
1fff1866:	e7f9      	b.n	1fff185c <Reset_Handler+0x1c>
3:
    bl trace_reset_marker
1fff1868:	f000 f858 	bl	1fff191c <trace_reset_marker>
    bl installer_main
1fff186c:	f000 f858 	bl	1fff1920 <installer_main>
4:
    b 4b
1fff1870:	e7fe      	b.n	1fff1870 <Reset_Handler+0x30>
1fff1872:	0000      	.short	0x0000
    ldr r0, =_stack_top
1fff1874:	1fffd000 	.word	0x1fffd000
    ldr r0, =_sbss
1fff1878:	1fff1ce8 	.word	0x1fff1ce8
    ldr r1, =_ebss
1fff187c:	1fff1ee8 	.word	0x1fff1ee8

1fff1880 <fail>:
}
#endif

static void status(uint32_t code)
{
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1880:	4b03      	ldr	r3, [pc, #12]	@ (1fff1890 <fail+0x10>)
1fff1882:	4a04      	ldr	r2, [pc, #16]	@ (1fff1894 <fail+0x14>)
1fff1884:	601a      	str	r2, [r3, #0]
    STATUS_CODE_REG = code;
1fff1886:	4b04      	ldr	r3, [pc, #16]	@ (1fff1898 <fail+0x18>)
1fff1888:	6018      	str	r0, [r3, #0]

static void fail(uint32_t code)
{
    status(code);
    for (;;) {
        __asm volatile("nop");
1fff188a:	46c0      	nop			@ (mov r8, r8)
    for (;;) {
1fff188c:	e7fd      	b.n	1fff188a <fail+0xa>
1fff188e:	46c0      	nop			@ (mov r8, r8)
1fff1890:	4000f0c8 	.word	0x4000f0c8
1fff1894:	33494249 	.word	0x33494249
1fff1898:	4000f0cc 	.word	0x4000f0cc

1fff189c <crc16_update>:
{
    return (value + align - 1u) & ~(align - 1u);
}

static uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint32_t size)
{
1fff189c:	b570      	push	{r4, r5, r6, lr}
    uint32_t bit;

    for (i = 0; i < size; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
1fff189e:	2501      	movs	r5, #1
1fff18a0:	4e09      	ldr	r6, [pc, #36]	@ (1fff18c8 <crc16_update+0x2c>)
1fff18a2:	188a      	adds	r2, r1, r2
        crc ^= data[i];
1fff18a4:	2408      	movs	r4, #8
1fff18a6:	780b      	ldrb	r3, [r1, #0]
1fff18a8:	4058      	eors	r0, r3
            if (crc & 1u) {
1fff18aa:	0003      	movs	r3, r0
1fff18ac:	402b      	ands	r3, r5
1fff18ae:	425b      	negs	r3, r3
1fff18b0:	4033      	ands	r3, r6
                crc = (uint16_t)((crc >> 1) ^ 0xa001u);
1fff18b2:	0840      	lsrs	r0, r0, #1
            if (crc & 1u) {
1fff18b4:	4058      	eors	r0, r3
        for (bit = 0; bit < 8; bit++) {
1fff18b6:	3c01      	subs	r4, #1
            if (crc & 1u) {
1fff18b8:	b280      	uxth	r0, r0
        for (bit = 0; bit < 8; bit++) {
1fff18ba:	2c00      	cmp	r4, #0
1fff18bc:	d1f5      	bne.n	1fff18aa <crc16_update+0xe>
    for (i = 0; i < size; i++) {
1fff18be:	3101      	adds	r1, #1
1fff18c0:	4291      	cmp	r1, r2
1fff18c2:	d1ef      	bne.n	1fff18a4 <crc16_update+0x8>
            }
        }
    }

    return crc;
}
1fff18c4:	bd70      	pop	{r4, r5, r6, pc}
1fff18c6:	46c0      	nop			@ (mov r8, r8)
1fff18c8:	ffffa001 	.word	0xffffa001

1fff18cc <crc16_flash>:

static uint16_t crc16_flash(uint32_t addr, uint32_t size)
{
1fff18cc:	b5f7      	push	{r0, r1, r2, r4, r5, r6, r7, lr}
1fff18ce:	0006      	movs	r6, r0
1fff18d0:	000c      	movs	r4, r1
    uint16_t crc = 0;
1fff18d2:	2500      	movs	r5, #0
    uint32_t todo;

    while (size) {
1fff18d4:	2c00      	cmp	r4, #0
1fff18d6:	d101      	bne.n	1fff18dc <crc16_flash+0x10>
        addr += todo;
        size -= todo;
    }

    return crc;
}
1fff18d8:	0028      	movs	r0, r5
1fff18da:	bdfe      	pop	{r1, r2, r3, r4, r5, r6, r7, pc}
        if (todo > sizeof(chunk)) {
1fff18dc:	2380      	movs	r3, #128	@ 0x80
1fff18de:	0027      	movs	r7, r4
1fff18e0:	005b      	lsls	r3, r3, #1
1fff18e2:	429c      	cmp	r4, r3
1fff18e4:	d900      	bls.n	1fff18e8 <crc16_flash+0x1c>
1fff18e6:	001f      	movs	r7, r3
        if (spif_read(addr, chunk, todo) != 0) {
1fff18e8:	4b0a      	ldr	r3, [pc, #40]	@ (1fff1914 <crc16_flash+0x48>)
1fff18ea:	003a      	movs	r2, r7
1fff18ec:	0019      	movs	r1, r3
1fff18ee:	0030      	movs	r0, r6
1fff18f0:	9301      	str	r3, [sp, #4]
1fff18f2:	f000 f9f1 	bl	1fff1cd8 <__spif_read_veneer>
1fff18f6:	2800      	cmp	r0, #0
1fff18f8:	d002      	beq.n	1fff1900 <crc16_flash+0x34>
            fail(0x1001u);
1fff18fa:	4807      	ldr	r0, [pc, #28]	@ (1fff1918 <crc16_flash+0x4c>)
1fff18fc:	f7ff ffc0 	bl	1fff1880 <fail>
        crc = crc16_update(crc, chunk, todo);
1fff1900:	0028      	movs	r0, r5
1fff1902:	003a      	movs	r2, r7
1fff1904:	9901      	ldr	r1, [sp, #4]
1fff1906:	f7ff ffc9 	bl	1fff189c <crc16_update>
        addr += todo;
1fff190a:	19f6      	adds	r6, r6, r7
        crc = crc16_update(crc, chunk, todo);
1fff190c:	0005      	movs	r5, r0
        size -= todo;
1fff190e:	1be4      	subs	r4, r4, r7
1fff1910:	e7e0      	b.n	1fff18d4 <crc16_flash+0x8>
1fff1912:	46c0      	nop			@ (mov r8, r8)
1fff1914:	1fff1ce8 	.word	0x1fff1ce8
1fff1918:	00001001 	.word	0x00001001

1fff191c <trace_reset_marker>:
}
1fff191c:	4770      	bx	lr
	...

1fff1920 <installer_main>:
        done += todo;
    }
}

void installer_main(void)
{
1fff1920:	b5f0      	push	{r4, r5, r6, r7, lr}
    uint32_t records_size;
    uint32_t i;
    uint16_t records_crc;

    status(PROGRESS(0x0001u));
    spif_config(SYS_CLK_DLL_64M, 1, XFRD_FCMD_READ_DUAL, 0, 0);
1fff1922:	2400      	movs	r4, #0
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1924:	4dbc      	ldr	r5, [pc, #752]	@ (1fff1c18 <installer_main+0x2f8>)
1fff1926:	4ebd      	ldr	r6, [pc, #756]	@ (1fff1c1c <installer_main+0x2fc>)
    STATUS_CODE_REG = code;
1fff1928:	4fbd      	ldr	r7, [pc, #756]	@ (1fff1c20 <installer_main+0x300>)
1fff192a:	4bbe      	ldr	r3, [pc, #760]	@ (1fff1c24 <installer_main+0x304>)
{
1fff192c:	b091      	sub	sp, #68	@ 0x44
    spif_config(SYS_CLK_DLL_64M, 1, XFRD_FCMD_READ_DUAL, 0, 0);
1fff192e:	2101      	movs	r1, #1
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1930:	602e      	str	r6, [r5, #0]
    spif_config(SYS_CLK_DLL_64M, 1, XFRD_FCMD_READ_DUAL, 0, 0);
1fff1932:	4abd      	ldr	r2, [pc, #756]	@ (1fff1c28 <installer_main+0x308>)
    STATUS_CODE_REG = code;
1fff1934:	603b      	str	r3, [r7, #0]
    spif_config(SYS_CLK_DLL_64M, 1, XFRD_FCMD_READ_DUAL, 0, 0);
1fff1936:	2004      	movs	r0, #4
1fff1938:	0023      	movs	r3, r4
1fff193a:	9400      	str	r4, [sp, #0]
1fff193c:	f000 f9c4 	bl	1fff1cc8 <__spif_config_veneer>
    AP_SPIF_WR_COMPLETION_CTRL = 0xff010005u;
1fff1940:	4bba      	ldr	r3, [pc, #744]	@ (1fff1c2c <installer_main+0x30c>)
1fff1942:	4abb      	ldr	r2, [pc, #748]	@ (1fff1c30 <installer_main+0x310>)
    AP_SPIF_LOW_WR_PROTECTION = 0;
    AP_SPIF_UP_WR_PROTECTION = 0x10u;
    AP_SPIF_WR_PROTECTION = 0x2u;
    spif_write_protect(0);
1fff1944:	0020      	movs	r0, r4
    AP_SPIF_WR_COMPLETION_CTRL = 0xff010005u;
1fff1946:	601a      	str	r2, [r3, #0]
    AP_SPIF_UP_WR_PROTECTION = 0x10u;
1fff1948:	2210      	movs	r2, #16
    AP_SPIF_LOW_WR_PROTECTION = 0;
1fff194a:	4bba      	ldr	r3, [pc, #744]	@ (1fff1c34 <installer_main+0x314>)
1fff194c:	601c      	str	r4, [r3, #0]
    AP_SPIF_UP_WR_PROTECTION = 0x10u;
1fff194e:	4bba      	ldr	r3, [pc, #744]	@ (1fff1c38 <installer_main+0x318>)
    status(PROGRESS(0x0010u));

    if (spif_read(IBI3_STAGING_ADDR, (uint8_t *)&hdr, sizeof(hdr)) != 0) {
1fff1950:	ac08      	add	r4, sp, #32
    AP_SPIF_UP_WR_PROTECTION = 0x10u;
1fff1952:	601a      	str	r2, [r3, #0]
    AP_SPIF_WR_PROTECTION = 0x2u;
1fff1954:	4bb9      	ldr	r3, [pc, #740]	@ (1fff1c3c <installer_main+0x31c>)
1fff1956:	3a0e      	subs	r2, #14
1fff1958:	601a      	str	r2, [r3, #0]
    spif_write_protect(0);
1fff195a:	f000 f9ad 	bl	1fff1cb8 <__spif_write_protect_veneer>
    STATUS_CODE_REG = code;
1fff195e:	4bb8      	ldr	r3, [pc, #736]	@ (1fff1c40 <installer_main+0x320>)
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1960:	602e      	str	r6, [r5, #0]
    if (spif_read(IBI3_STAGING_ADDR, (uint8_t *)&hdr, sizeof(hdr)) != 0) {
1fff1962:	2220      	movs	r2, #32
1fff1964:	0021      	movs	r1, r4
1fff1966:	48b7      	ldr	r0, [pc, #732]	@ (1fff1c44 <installer_main+0x324>)
    STATUS_CODE_REG = code;
1fff1968:	603b      	str	r3, [r7, #0]
    if (spif_read(IBI3_STAGING_ADDR, (uint8_t *)&hdr, sizeof(hdr)) != 0) {
1fff196a:	f000 f9b5 	bl	1fff1cd8 <__spif_read_veneer>
1fff196e:	2800      	cmp	r0, #0
1fff1970:	d002      	beq.n	1fff1978 <installer_main+0x58>
        fail(0x0100u);
1fff1972:	2080      	movs	r0, #128	@ 0x80
    if (spif_read(IBI3_STAGING_ADDR + sizeof(hdr), (uint8_t *)records, records_size) != 0) {
        fail(0x0105u);
    }
    records_crc = crc16_update(0, (uint8_t *)records, records_size);
    if (records_crc != (uint16_t)hdr.records_crc16) {
        fail(0x0106u);
1fff1974:	0040      	lsls	r0, r0, #1
1fff1976:	e008      	b.n	1fff198a <installer_main+0x6a>
    STATUS_CODE_REG = code;
1fff1978:	4bb3      	ldr	r3, [pc, #716]	@ (1fff1c48 <installer_main+0x328>)
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff197a:	4aa8      	ldr	r2, [pc, #672]	@ (1fff1c1c <installer_main+0x2fc>)
1fff197c:	602a      	str	r2, [r5, #0]
    STATUS_CODE_REG = code;
1fff197e:	603b      	str	r3, [r7, #0]
    if (hdr.magic != IBI3_MAGIC) {
1fff1980:	9b08      	ldr	r3, [sp, #32]
1fff1982:	4293      	cmp	r3, r2
1fff1984:	d003      	beq.n	1fff198e <installer_main+0x6e>
        fail(0x0101u);
1fff1986:	2002      	movs	r0, #2
1fff1988:	30ff      	adds	r0, #255	@ 0xff
1fff198a:	f7ff ff79 	bl	1fff1880 <fail>
    if (hdr.version != IBI3_VERSION) {
1fff198e:	6863      	ldr	r3, [r4, #4]
        fail(0x0102u);
1fff1990:	2081      	movs	r0, #129	@ 0x81
    if (hdr.version != IBI3_VERSION) {
1fff1992:	2b01      	cmp	r3, #1
1fff1994:	d1ee      	bne.n	1fff1974 <installer_main+0x54>
    if (hdr.record_count == 0 || hdr.record_count > IBI3_MAX_RECORDS) {
1fff1996:	68a6      	ldr	r6, [r4, #8]
        fail(0x0103u);
1fff1998:	2004      	movs	r0, #4
    if (hdr.record_count == 0 || hdr.record_count > IBI3_MAX_RECORDS) {
1fff199a:	1e73      	subs	r3, r6, #1
1fff199c:	2b0f      	cmp	r3, #15
1fff199e:	d8f3      	bhi.n	1fff1988 <installer_main+0x68>
    records_size = hdr.record_count * sizeof(records[0]);
1fff19a0:	0136      	lsls	r6, r6, #4
    if (hdr.header_size < sizeof(hdr) + records_size ||
1fff19a2:	0032      	movs	r2, r6
1fff19a4:	68e3      	ldr	r3, [r4, #12]
1fff19a6:	3220      	adds	r2, #32
1fff19a8:	4293      	cmp	r3, r2
1fff19aa:	d30a      	bcc.n	1fff19c2 <installer_main+0xa2>
1fff19ac:	2280      	movs	r2, #128	@ 0x80
1fff19ae:	0152      	lsls	r2, r2, #5
1fff19b0:	4293      	cmp	r3, r2
1fff19b2:	d806      	bhi.n	1fff19c2 <installer_main+0xa2>
        hdr.image_size <= hdr.header_size ||
1fff19b4:	6922      	ldr	r2, [r4, #16]
        hdr.header_size > 0x1000u ||
1fff19b6:	4293      	cmp	r3, r2
1fff19b8:	d203      	bcs.n	1fff19c2 <installer_main+0xa2>
        hdr.image_size <= hdr.header_size ||
1fff19ba:	2390      	movs	r3, #144	@ 0x90
1fff19bc:	025b      	lsls	r3, r3, #9
1fff19be:	429a      	cmp	r2, r3
1fff19c0:	d901      	bls.n	1fff19c6 <installer_main+0xa6>
        fail(0x0104u);
1fff19c2:	2082      	movs	r0, #130	@ 0x82
1fff19c4:	e7d6      	b.n	1fff1974 <installer_main+0x54>
    if (spif_read(IBI3_STAGING_ADDR + sizeof(hdr), (uint8_t *)records, records_size) != 0) {
1fff19c6:	4ba1      	ldr	r3, [pc, #644]	@ (1fff1c4c <installer_main+0x32c>)
1fff19c8:	0032      	movs	r2, r6
1fff19ca:	0019      	movs	r1, r3
1fff19cc:	48a0      	ldr	r0, [pc, #640]	@ (1fff1c50 <installer_main+0x330>)
1fff19ce:	9303      	str	r3, [sp, #12]
1fff19d0:	f000 f982 	bl	1fff1cd8 <__spif_read_veneer>
1fff19d4:	2800      	cmp	r0, #0
1fff19d6:	d001      	beq.n	1fff19dc <installer_main+0xbc>
        fail(0x0105u);
1fff19d8:	2006      	movs	r0, #6
1fff19da:	e7d5      	b.n	1fff1988 <installer_main+0x68>
    records_crc = crc16_update(0, (uint8_t *)records, records_size);
1fff19dc:	0032      	movs	r2, r6
1fff19de:	9903      	ldr	r1, [sp, #12]
1fff19e0:	f7ff ff5c 	bl	1fff189c <crc16_update>
    if (records_crc != (uint16_t)hdr.records_crc16) {
1fff19e4:	6963      	ldr	r3, [r4, #20]
1fff19e6:	b29b      	uxth	r3, r3
1fff19e8:	4283      	cmp	r3, r0
1fff19ea:	d001      	beq.n	1fff19f0 <installer_main+0xd0>
        fail(0x0106u);
1fff19ec:	2083      	movs	r0, #131	@ 0x83
1fff19ee:	e7c1      	b.n	1fff1974 <installer_main+0x54>
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff19f0:	4b8a      	ldr	r3, [pc, #552]	@ (1fff1c1c <installer_main+0x2fc>)
    }
    status(PROGRESS(0x0021u));

    for (i = 0; i < hdr.record_count; i++) {
1fff19f2:	2400      	movs	r4, #0
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff19f4:	602b      	str	r3, [r5, #0]
    STATUS_CODE_REG = code;
1fff19f6:	4b97      	ldr	r3, [pc, #604]	@ (1fff1c54 <installer_main+0x334>)
1fff19f8:	603b      	str	r3, [r7, #0]
    for (i = 0; i < hdr.record_count; i++) {
1fff19fa:	9b03      	ldr	r3, [sp, #12]
1fff19fc:	001e      	movs	r6, r3
1fff19fe:	9305      	str	r3, [sp, #20]
1fff1a00:	9b0a      	ldr	r3, [sp, #40]	@ 0x28
1fff1a02:	42a3      	cmp	r3, r4
1fff1a04:	d81e      	bhi.n	1fff1a44 <installer_main+0x124>
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1a06:	4b85      	ldr	r3, [pc, #532]	@ (1fff1c1c <installer_main+0x2fc>)
    for (i = 0; i < hdr->record_count; i++) {
1fff1a08:	2000      	movs	r0, #0
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1a0a:	602b      	str	r3, [r5, #0]
    STATUS_CODE_REG = code;
1fff1a0c:	4b92      	ldr	r3, [pc, #584]	@ (1fff1c58 <installer_main+0x338>)
    for (i = 0; i < hdr->record_count; i++) {
1fff1a0e:	9903      	ldr	r1, [sp, #12]
    STATUS_CODE_REG = code;
1fff1a10:	603b      	str	r3, [r7, #0]
    for (i = 0; i < hdr->record_count; i++) {
1fff1a12:	9b0a      	ldr	r3, [sp, #40]	@ 0x28
    uint32_t min_addr = FLASH_END;
1fff1a14:	4c91      	ldr	r4, [pc, #580]	@ (1fff1c5c <installer_main+0x33c>)
    for (i = 0; i < hdr->record_count; i++) {
1fff1a16:	469c      	mov	ip, r3
    uint32_t max_addr = FLASH_BASE;
1fff1a18:	2388      	movs	r3, #136	@ 0x88
1fff1a1a:	055b      	lsls	r3, r3, #21
    for (i = 0; i < hdr->record_count; i++) {
1fff1a1c:	4560      	cmp	r0, ip
1fff1a1e:	d157      	bne.n	1fff1ad0 <installer_main+0x1b0>
    return (value + align - 1u) & ~(align - 1u);
1fff1a20:	4a8f      	ldr	r2, [pc, #572]	@ (1fff1c60 <installer_main+0x340>)
    return value & ~(align - 1u);
1fff1a22:	0b24      	lsrs	r4, r4, #12
    return (value + align - 1u) & ~(align - 1u);
1fff1a24:	189b      	adds	r3, r3, r2
1fff1a26:	0b1b      	lsrs	r3, r3, #12
1fff1a28:	031b      	lsls	r3, r3, #12
    return value & ~(align - 1u);
1fff1a2a:	0324      	lsls	r4, r4, #12
    return (value + align - 1u) & ~(align - 1u);
1fff1a2c:	9304      	str	r3, [sp, #16]
    for (sector = min_addr; sector < max_addr; sector += FLASH_SECTOR_SIZE) {
1fff1a2e:	9b04      	ldr	r3, [sp, #16]
1fff1a30:	42a3      	cmp	r3, r4
1fff1a32:	d968      	bls.n	1fff1b06 <installer_main+0x1e6>
    return rec->target_addr < sector_end && rec_end > sector;
1fff1a34:	2280      	movs	r2, #128	@ 0x80
        for (i = 0; i < hdr->record_count; i++) {
1fff1a36:	9b0a      	ldr	r3, [sp, #40]	@ 0x28
    return rec->target_addr < sector_end && rec_end > sector;
1fff1a38:	0152      	lsls	r2, r2, #5
        for (i = 0; i < hdr->record_count; i++) {
1fff1a3a:	469c      	mov	ip, r3
1fff1a3c:	2100      	movs	r1, #0
1fff1a3e:	9b03      	ldr	r3, [sp, #12]
    return rec->target_addr < sector_end && rec_end > sector;
1fff1a40:	18a0      	adds	r0, r4, r2
1fff1a42:	e05a      	b.n	1fff1afa <installer_main+0x1da>
        status(PROGRESS(0x0100u | i));
1fff1a44:	4b87      	ldr	r3, [pc, #540]	@ (1fff1c64 <installer_main+0x344>)
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1a46:	4a75      	ldr	r2, [pc, #468]	@ (1fff1c1c <installer_main+0x2fc>)
        status(PROGRESS(0x0100u | i));
1fff1a48:	4323      	orrs	r3, r4
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1a4a:	602a      	str	r2, [r5, #0]
    STATUS_CODE_REG = code;
1fff1a4c:	603b      	str	r3, [r7, #0]
    if (rec->size == 0) {
1fff1a4e:	68b1      	ldr	r1, [r6, #8]
1fff1a50:	9606      	str	r6, [sp, #24]
1fff1a52:	2900      	cmp	r1, #0
1fff1a54:	d103      	bne.n	1fff1a5e <installer_main+0x13e>
        fail(0x2000u | index);
1fff1a56:	2080      	movs	r0, #128	@ 0x80
        fail(0x2100u | index);
1fff1a58:	0180      	lsls	r0, r0, #6
    }

    for (i = 0; i < hdr.record_count; i++) {
        status(PROGRESS(0x0400u | i));
        if (crc16_flash(records[i].target_addr, records[i].size) != (uint16_t)records[i].crc16) {
            fail(0x5000u | i);
1fff1a5a:	4320      	orrs	r0, r4
1fff1a5c:	e795      	b.n	1fff198a <installer_main+0x6a>
    if (add_overflow(IBI3_STAGING_ADDR, rec->source_offset) ||
1fff1a5e:	6873      	ldr	r3, [r6, #4]
1fff1a60:	4a81      	ldr	r2, [pc, #516]	@ (1fff1c68 <installer_main+0x348>)
1fff1a62:	4293      	cmp	r3, r2
1fff1a64:	d807      	bhi.n	1fff1a76 <installer_main+0x156>
    return b > (0xffffffffu - a);
1fff1a66:	43da      	mvns	r2, r3
    if (add_overflow(IBI3_STAGING_ADDR, rec->source_offset) ||
1fff1a68:	4291      	cmp	r1, r2
1fff1a6a:	d804      	bhi.n	1fff1a76 <installer_main+0x156>
        add_overflow(rec->target_addr, rec->size)) {
1fff1a6c:	6832      	ldr	r2, [r6, #0]
    return b > (0xffffffffu - a);
1fff1a6e:	43d0      	mvns	r0, r2
        add_overflow(rec->target_addr, rec->size)) {
1fff1a70:	9204      	str	r2, [sp, #16]
        add_overflow(rec->source_offset, rec->size) ||
1fff1a72:	4281      	cmp	r1, r0
1fff1a74:	d901      	bls.n	1fff1a7a <installer_main+0x15a>
        fail(0x2100u | index);
1fff1a76:	2084      	movs	r0, #132	@ 0x84
1fff1a78:	e7ee      	b.n	1fff1a58 <installer_main+0x138>
    source_addr = IBI3_STAGING_ADDR + rec->source_offset;
1fff1a7a:	4872      	ldr	r0, [pc, #456]	@ (1fff1c44 <installer_main+0x324>)
1fff1a7c:	1818      	adds	r0, r3, r0
    source_end = source_addr + rec->size;
1fff1a7e:	180a      	adds	r2, r1, r0
1fff1a80:	9207      	str	r2, [sp, #28]
    target_end = rec->target_addr + rec->size;
1fff1a82:	9a04      	ldr	r2, [sp, #16]
1fff1a84:	188a      	adds	r2, r1, r2
1fff1a86:	4694      	mov	ip, r2
    if (rec->source_offset < hdr->header_size ||
1fff1a88:	9a0b      	ldr	r2, [sp, #44]	@ 0x2c
1fff1a8a:	4293      	cmp	r3, r2
1fff1a8c:	d303      	bcc.n	1fff1a96 <installer_main+0x176>
1fff1a8e:	9a0c      	ldr	r2, [sp, #48]	@ 0x30
        rec->source_offset + rec->size > hdr->image_size) {
1fff1a90:	18cb      	adds	r3, r1, r3
    if (rec->source_offset < hdr->header_size ||
1fff1a92:	4293      	cmp	r3, r2
1fff1a94:	d901      	bls.n	1fff1a9a <installer_main+0x17a>
        fail(0x2200u | index);
1fff1a96:	2088      	movs	r0, #136	@ 0x88
1fff1a98:	e7de      	b.n	1fff1a58 <installer_main+0x138>
    if (source_addr < IBI3_STAGING_ADDR || source_end > FLASH_END) {
1fff1a9a:	4a70      	ldr	r2, [pc, #448]	@ (1fff1c5c <installer_main+0x33c>)
1fff1a9c:	180b      	adds	r3, r1, r0
1fff1a9e:	4293      	cmp	r3, r2
1fff1aa0:	d901      	bls.n	1fff1aa6 <installer_main+0x186>
        fail(0x2300u | index);
1fff1aa2:	208c      	movs	r0, #140	@ 0x8c
1fff1aa4:	e7d8      	b.n	1fff1a58 <installer_main+0x138>
    if (rec->target_addr < TARGET_MIN || target_end > TARGET_MAX) {
1fff1aa6:	4b71      	ldr	r3, [pc, #452]	@ (1fff1c6c <installer_main+0x34c>)
1fff1aa8:	9a04      	ldr	r2, [sp, #16]
1fff1aaa:	429a      	cmp	r2, r3
1fff1aac:	d902      	bls.n	1fff1ab4 <installer_main+0x194>
1fff1aae:	4b65      	ldr	r3, [pc, #404]	@ (1fff1c44 <installer_main+0x324>)
1fff1ab0:	459c      	cmp	ip, r3
1fff1ab2:	d901      	bls.n	1fff1ab8 <installer_main+0x198>
        fail(0x2400u | index);
1fff1ab4:	2090      	movs	r0, #144	@ 0x90
1fff1ab6:	e7cf      	b.n	1fff1a58 <installer_main+0x138>
    crc = crc16_flash(source_addr, rec->size);
1fff1ab8:	f7ff ff08 	bl	1fff18cc <crc16_flash>
    if (crc != (uint16_t)rec->crc16) {
1fff1abc:	9b06      	ldr	r3, [sp, #24]
1fff1abe:	3610      	adds	r6, #16
1fff1ac0:	68db      	ldr	r3, [r3, #12]
1fff1ac2:	b29b      	uxth	r3, r3
1fff1ac4:	4283      	cmp	r3, r0
1fff1ac6:	d001      	beq.n	1fff1acc <installer_main+0x1ac>
        fail(0x2600u | index);
1fff1ac8:	2098      	movs	r0, #152	@ 0x98
1fff1aca:	e7c5      	b.n	1fff1a58 <installer_main+0x138>
    for (i = 0; i < hdr.record_count; i++) {
1fff1acc:	3401      	adds	r4, #1
1fff1ace:	e797      	b.n	1fff1a00 <installer_main+0xe0>
        if (records[i].target_addr < min_addr) {
1fff1ad0:	680a      	ldr	r2, [r1, #0]
1fff1ad2:	4294      	cmp	r4, r2
1fff1ad4:	d900      	bls.n	1fff1ad8 <installer_main+0x1b8>
1fff1ad6:	0014      	movs	r4, r2
        if (records[i].target_addr + records[i].size > max_addr) {
1fff1ad8:	688e      	ldr	r6, [r1, #8]
1fff1ada:	1992      	adds	r2, r2, r6
1fff1adc:	4293      	cmp	r3, r2
1fff1ade:	d200      	bcs.n	1fff1ae2 <installer_main+0x1c2>
1fff1ae0:	0013      	movs	r3, r2
    for (i = 0; i < hdr->record_count; i++) {
1fff1ae2:	3001      	adds	r0, #1
1fff1ae4:	3110      	adds	r1, #16
1fff1ae6:	e799      	b.n	1fff1a1c <installer_main+0xfc>
    uint32_t rec_end = rec->target_addr + rec->size;
1fff1ae8:	681a      	ldr	r2, [r3, #0]
    return rec->target_addr < sector_end && rec_end > sector;
1fff1aea:	4282      	cmp	r2, r0
1fff1aec:	d203      	bcs.n	1fff1af6 <installer_main+0x1d6>
    uint32_t rec_end = rec->target_addr + rec->size;
1fff1aee:	689e      	ldr	r6, [r3, #8]
1fff1af0:	1992      	adds	r2, r2, r6
    return rec->target_addr < sector_end && rec_end > sector;
1fff1af2:	4294      	cmp	r4, r2
1fff1af4:	d37d      	bcc.n	1fff1bf2 <installer_main+0x2d2>
        for (i = 0; i < hdr->record_count; i++) {
1fff1af6:	3101      	adds	r1, #1
1fff1af8:	3310      	adds	r3, #16
1fff1afa:	4561      	cmp	r1, ip
1fff1afc:	d1f4      	bne.n	1fff1ae8 <installer_main+0x1c8>
    for (sector = min_addr; sector < max_addr; sector += FLASH_SECTOR_SIZE) {
1fff1afe:	2380      	movs	r3, #128	@ 0x80
1fff1b00:	015b      	lsls	r3, r3, #5
1fff1b02:	18e4      	adds	r4, r4, r3
1fff1b04:	e793      	b.n	1fff1a2e <installer_main+0x10e>
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1b06:	4b45      	ldr	r3, [pc, #276]	@ (1fff1c1c <installer_main+0x2fc>)
1fff1b08:	602b      	str	r3, [r5, #0]
    STATUS_CODE_REG = code;
1fff1b0a:	4b59      	ldr	r3, [pc, #356]	@ (1fff1c70 <installer_main+0x350>)
1fff1b0c:	603b      	str	r3, [r7, #0]
    for (i = 0; i < hdr.record_count; i++) {
1fff1b0e:	9b03      	ldr	r3, [sp, #12]
1fff1b10:	9304      	str	r3, [sp, #16]
1fff1b12:	2300      	movs	r3, #0
1fff1b14:	9303      	str	r3, [sp, #12]
1fff1b16:	9a03      	ldr	r2, [sp, #12]
1fff1b18:	9b0a      	ldr	r3, [sp, #40]	@ 0x28
1fff1b1a:	ae08      	add	r6, sp, #32
1fff1b1c:	4293      	cmp	r3, r2
1fff1b1e:	d812      	bhi.n	1fff1b46 <installer_main+0x226>
    for (i = 0; i < hdr.record_count; i++) {
1fff1b20:	2400      	movs	r4, #0
1fff1b22:	68b3      	ldr	r3, [r6, #8]
1fff1b24:	42a3      	cmp	r3, r4
1fff1b26:	d84d      	bhi.n	1fff1bc4 <installer_main+0x2a4>
        }
    }

    status(PROGRESS(0x0004u));
    OTA_MODE_SELECT_REG = 0;
1fff1b28:	2200      	movs	r2, #0
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1b2a:	493c      	ldr	r1, [pc, #240]	@ (1fff1c1c <installer_main+0x2fc>)
    STATUS_CODE_REG = code;
1fff1b2c:	4b51      	ldr	r3, [pc, #324]	@ (1fff1c74 <installer_main+0x354>)
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1b2e:	6029      	str	r1, [r5, #0]
    STATUS_CODE_REG = code;
1fff1b30:	603b      	str	r3, [r7, #0]
    OTA_MODE_SELECT_REG = 0;
1fff1b32:	4b51      	ldr	r3, [pc, #324]	@ (1fff1c78 <installer_main+0x358>)
1fff1b34:	601a      	str	r2, [r3, #0]
    STATUS_CODE_REG = code;
1fff1b36:	4b51      	ldr	r3, [pc, #324]	@ (1fff1c7c <installer_main+0x35c>)
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1b38:	6029      	str	r1, [r5, #0]
    status(PROGRESS(0x0005u));
    SCB_AIRCR = 0x05fa0004u;
1fff1b3a:	4a51      	ldr	r2, [pc, #324]	@ (1fff1c80 <installer_main+0x360>)
    STATUS_CODE_REG = code;
1fff1b3c:	603b      	str	r3, [r7, #0]
    SCB_AIRCR = 0x05fa0004u;
1fff1b3e:	4b51      	ldr	r3, [pc, #324]	@ (1fff1c84 <installer_main+0x364>)
1fff1b40:	601a      	str	r2, [r3, #0]
    for (;;) {
        __asm volatile("nop");
1fff1b42:	46c0      	nop			@ (mov r8, r8)
    for (;;) {
1fff1b44:	e7fd      	b.n	1fff1b42 <installer_main+0x222>
    uint32_t done = 0;
1fff1b46:	2600      	movs	r6, #0
        status(PROGRESS(0x0300u | i));
1fff1b48:	9a03      	ldr	r2, [sp, #12]
1fff1b4a:	4b4f      	ldr	r3, [pc, #316]	@ (1fff1c88 <installer_main+0x368>)
1fff1b4c:	4313      	orrs	r3, r2
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1b4e:	4a33      	ldr	r2, [pc, #204]	@ (1fff1c1c <installer_main+0x2fc>)
1fff1b50:	602a      	str	r2, [r5, #0]
    STATUS_CODE_REG = code;
1fff1b52:	603b      	str	r3, [r7, #0]
    while (done < rec->size) {
1fff1b54:	9b04      	ldr	r3, [sp, #16]
1fff1b56:	689b      	ldr	r3, [r3, #8]
1fff1b58:	42b3      	cmp	r3, r6
1fff1b5a:	d806      	bhi.n	1fff1b6a <installer_main+0x24a>
    for (i = 0; i < hdr.record_count; i++) {
1fff1b5c:	9b03      	ldr	r3, [sp, #12]
1fff1b5e:	3301      	adds	r3, #1
1fff1b60:	9303      	str	r3, [sp, #12]
1fff1b62:	9b04      	ldr	r3, [sp, #16]
1fff1b64:	3310      	adds	r3, #16
1fff1b66:	9304      	str	r3, [sp, #16]
1fff1b68:	e7d5      	b.n	1fff1b16 <installer_main+0x1f6>
        page_left = 256u - ((rec->target_addr + done) & 0xffu);
1fff1b6a:	9a04      	ldr	r2, [sp, #16]
        todo = rec->size - done;
1fff1b6c:	1b9b      	subs	r3, r3, r6
        page_left = 256u - ((rec->target_addr + done) & 0xffu);
1fff1b6e:	6814      	ldr	r4, [r2, #0]
1fff1b70:	22ff      	movs	r2, #255	@ 0xff
1fff1b72:	1934      	adds	r4, r6, r4
1fff1b74:	4014      	ands	r4, r2
1fff1b76:	3201      	adds	r2, #1
1fff1b78:	1b14      	subs	r4, r2, r4
        if (todo > sizeof(chunk)) {
1fff1b7a:	4293      	cmp	r3, r2
1fff1b7c:	d900      	bls.n	1fff1b80 <installer_main+0x260>
1fff1b7e:	0013      	movs	r3, r2
        if (todo > page_left) {
1fff1b80:	429c      	cmp	r4, r3
1fff1b82:	d900      	bls.n	1fff1b86 <installer_main+0x266>
1fff1b84:	001c      	movs	r4, r3
        if (spif_read(IBI3_STAGING_ADDR + rec->source_offset + done, chunk, todo) != 0) {
1fff1b86:	4b41      	ldr	r3, [pc, #260]	@ (1fff1c8c <installer_main+0x36c>)
1fff1b88:	0022      	movs	r2, r4
1fff1b8a:	9306      	str	r3, [sp, #24]
1fff1b8c:	9b04      	ldr	r3, [sp, #16]
1fff1b8e:	9906      	ldr	r1, [sp, #24]
1fff1b90:	6858      	ldr	r0, [r3, #4]
1fff1b92:	4b2c      	ldr	r3, [pc, #176]	@ (1fff1c44 <installer_main+0x324>)
1fff1b94:	18c0      	adds	r0, r0, r3
1fff1b96:	1980      	adds	r0, r0, r6
1fff1b98:	f000 f89e 	bl	1fff1cd8 <__spif_read_veneer>
1fff1b9c:	2800      	cmp	r0, #0
1fff1b9e:	d004      	beq.n	1fff1baa <installer_main+0x28a>
            fail(0x4000u | index);
1fff1ba0:	2080      	movs	r0, #128	@ 0x80
            fail(0x4100u | index);
1fff1ba2:	9b03      	ldr	r3, [sp, #12]
1fff1ba4:	01c0      	lsls	r0, r0, #7
1fff1ba6:	4318      	orrs	r0, r3
1fff1ba8:	e6ef      	b.n	1fff198a <installer_main+0x6a>
        if (spif_write(rec->target_addr + done, chunk, todo) != 0) {
1fff1baa:	9b04      	ldr	r3, [sp, #16]
1fff1bac:	0022      	movs	r2, r4
1fff1bae:	6818      	ldr	r0, [r3, #0]
1fff1bb0:	9906      	ldr	r1, [sp, #24]
1fff1bb2:	1830      	adds	r0, r6, r0
1fff1bb4:	f000 f870 	bl	1fff1c98 <__spif_write_veneer>
1fff1bb8:	2800      	cmp	r0, #0
1fff1bba:	d001      	beq.n	1fff1bc0 <installer_main+0x2a0>
            fail(0x4100u | index);
1fff1bbc:	2082      	movs	r0, #130	@ 0x82
1fff1bbe:	e7f0      	b.n	1fff1ba2 <installer_main+0x282>
        done += todo;
1fff1bc0:	1936      	adds	r6, r6, r4
1fff1bc2:	e7c7      	b.n	1fff1b54 <installer_main+0x234>
        status(PROGRESS(0x0400u | i));
1fff1bc4:	4b32      	ldr	r3, [pc, #200]	@ (1fff1c90 <installer_main+0x370>)
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1bc6:	4a15      	ldr	r2, [pc, #84]	@ (1fff1c1c <installer_main+0x2fc>)
        status(PROGRESS(0x0400u | i));
1fff1bc8:	4323      	orrs	r3, r4
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1bca:	602a      	str	r2, [r5, #0]
    STATUS_CODE_REG = code;
1fff1bcc:	603b      	str	r3, [r7, #0]
        if (crc16_flash(records[i].target_addr, records[i].size) != (uint16_t)records[i].crc16) {
1fff1bce:	9b05      	ldr	r3, [sp, #20]
1fff1bd0:	6899      	ldr	r1, [r3, #8]
1fff1bd2:	6818      	ldr	r0, [r3, #0]
1fff1bd4:	f7ff fe7a 	bl	1fff18cc <crc16_flash>
1fff1bd8:	9b05      	ldr	r3, [sp, #20]
1fff1bda:	9a05      	ldr	r2, [sp, #20]
1fff1bdc:	68db      	ldr	r3, [r3, #12]
1fff1bde:	3210      	adds	r2, #16
1fff1be0:	b29b      	uxth	r3, r3
1fff1be2:	9205      	str	r2, [sp, #20]
1fff1be4:	4283      	cmp	r3, r0
1fff1be6:	d002      	beq.n	1fff1bee <installer_main+0x2ce>
            fail(0x5000u | i);
1fff1be8:	20a0      	movs	r0, #160	@ 0xa0
1fff1bea:	01c0      	lsls	r0, r0, #7
1fff1bec:	e735      	b.n	1fff1a5a <installer_main+0x13a>
    for (i = 0; i < hdr.record_count; i++) {
1fff1bee:	3401      	adds	r4, #1
1fff1bf0:	e797      	b.n	1fff1b22 <installer_main+0x202>
            status(PROGRESS(0x0200u | ((sector >> 12) & 0xffu)));
1fff1bf2:	23ff      	movs	r3, #255	@ 0xff
1fff1bf4:	0b26      	lsrs	r6, r4, #12
1fff1bf6:	401e      	ands	r6, r3
1fff1bf8:	4b26      	ldr	r3, [pc, #152]	@ (1fff1c94 <installer_main+0x374>)
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1bfa:	4a08      	ldr	r2, [pc, #32]	@ (1fff1c1c <installer_main+0x2fc>)
            status(PROGRESS(0x0200u | ((sector >> 12) & 0xffu)));
1fff1bfc:	4333      	orrs	r3, r6
    STATUS_MAGIC_REG = IBI3_MAGIC;
1fff1bfe:	602a      	str	r2, [r5, #0]
        if (hit && spif_erase_sector(sector) != 0) {
1fff1c00:	0020      	movs	r0, r4
    STATUS_CODE_REG = code;
1fff1c02:	603b      	str	r3, [r7, #0]
        if (hit && spif_erase_sector(sector) != 0) {
1fff1c04:	f000 f850 	bl	1fff1ca8 <__spif_erase_sector_veneer>
1fff1c08:	2800      	cmp	r0, #0
1fff1c0a:	d100      	bne.n	1fff1c0e <installer_main+0x2ee>
1fff1c0c:	e777      	b.n	1fff1afe <installer_main+0x1de>
            fail(0x3000u | ((sector >> 12) & 0xffu));
1fff1c0e:	20c0      	movs	r0, #192	@ 0xc0
1fff1c10:	0180      	lsls	r0, r0, #6
1fff1c12:	4330      	orrs	r0, r6
1fff1c14:	e6b9      	b.n	1fff198a <installer_main+0x6a>
1fff1c16:	46c0      	nop			@ (mov r8, r8)
1fff1c18:	4000f0c8 	.word	0x4000f0c8
1fff1c1c:	33494249 	.word	0x33494249
1fff1c20:	4000f0cc 	.word	0x4000f0cc
1fff1c24:	80000001 	.word	0x80000001
1fff1c28:	0801003b 	.word	0x0801003b
1fff1c2c:	4000c838 	.word	0x4000c838
1fff1c30:	ff010005 	.word	0xff010005
1fff1c34:	4000c850 	.word	0x4000c850
1fff1c38:	4000c854 	.word	0x4000c854
1fff1c3c:	4000c858 	.word	0x4000c858
1fff1c40:	80000010 	.word	0x80000010
1fff1c44:	1106e000 	.word	0x1106e000
1fff1c48:	80000020 	.word	0x80000020
1fff1c4c:	1fff1de8 	.word	0x1fff1de8
1fff1c50:	1106e020 	.word	0x1106e020
1fff1c54:	80000021 	.word	0x80000021
1fff1c58:	80000002 	.word	0x80000002
1fff1c5c:	11080000 	.word	0x11080000
1fff1c60:	00000fff 	.word	0x00000fff
1fff1c64:	80000100 	.word	0x80000100
1fff1c68:	eef91fff 	.word	0xeef91fff
1fff1c6c:	11001fff 	.word	0x11001fff
1fff1c70:	80000003 	.word	0x80000003
1fff1c74:	80000004 	.word	0x80000004
1fff1c78:	4000f034 	.word	0x4000f034
1fff1c7c:	80000005 	.word	0x80000005
1fff1c80:	05fa0004 	.word	0x05fa0004
1fff1c84:	e000ed0c 	.word	0xe000ed0c
1fff1c88:	80000300 	.word	0x80000300
1fff1c8c:	1fff1ce8 	.word	0x1fff1ce8
1fff1c90:	80000400 	.word	0x80000400
1fff1c94:	80000200 	.word	0x80000200

1fff1c98 <__spif_write_veneer>:
1fff1c98:	b401      	push	{r0}
1fff1c9a:	4802      	ldr	r0, [pc, #8]	@ (1fff1ca4 <__spif_write_veneer+0xc>)
1fff1c9c:	4684      	mov	ip, r0
1fff1c9e:	bc01      	pop	{r0}
1fff1ca0:	4760      	bx	ip
1fff1ca2:	bf00      	nop
1fff1ca4:	00017395 	.word	0x00017395

1fff1ca8 <__spif_erase_sector_veneer>:
1fff1ca8:	b401      	push	{r0}
1fff1caa:	4802      	ldr	r0, [pc, #8]	@ (1fff1cb4 <__spif_erase_sector_veneer+0xc>)
1fff1cac:	4684      	mov	ip, r0
1fff1cae:	bc01      	pop	{r0}
1fff1cb0:	4760      	bx	ip
1fff1cb2:	bf00      	nop
1fff1cb4:	00016fa9 	.word	0x00016fa9

1fff1cb8 <__spif_write_protect_veneer>:
1fff1cb8:	b401      	push	{r0}
1fff1cba:	4802      	ldr	r0, [pc, #8]	@ (1fff1cc4 <__spif_write_protect_veneer+0xc>)
1fff1cbc:	4684      	mov	ip, r0
1fff1cbe:	bc01      	pop	{r0}
1fff1cc0:	4760      	bx	ip
1fff1cc2:	bf00      	nop
1fff1cc4:	000174f9 	.word	0x000174f9

1fff1cc8 <__spif_config_veneer>:
1fff1cc8:	b401      	push	{r0}
1fff1cca:	4802      	ldr	r0, [pc, #8]	@ (1fff1cd4 <__spif_config_veneer+0xc>)
1fff1ccc:	4684      	mov	ip, r0
1fff1cce:	bc01      	pop	{r0}
1fff1cd0:	4760      	bx	ip
1fff1cd2:	bf00      	nop
1fff1cd4:	00016dc5 	.word	0x00016dc5

1fff1cd8 <__spif_read_veneer>:
1fff1cd8:	b401      	push	{r0}
1fff1cda:	4802      	ldr	r0, [pc, #8]	@ (1fff1ce4 <__spif_read_veneer+0xc>)
1fff1cdc:	4684      	mov	ip, r0
1fff1cde:	bc01      	pop	{r0}
1fff1ce0:	4760      	bx	ip
1fff1ce2:	bf00      	nop
1fff1ce4:	00017165 	.word	0x00017165
