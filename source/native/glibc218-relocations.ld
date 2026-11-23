/* The TV's old loader merges .rel.dyn and .rel.plt into one range.
 * Keep unwind tables outside that range when linking with modern LLD. */
SECTIONS { .rel.plt : { *(.rel.plt) *(.rel.iplt) } }
INSERT AFTER .rel.dyn;
