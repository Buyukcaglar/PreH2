"""p2hints.py - facts about PRE2.EXE's code segment that control-flow analysis cannot find.

ENTRIES: extra code entry points: interrupt handlers installed through stored offsets, and the
targets of the jump tables (read from the image):
  cs:6AA9 (13 words, call at 68FC), cs:6AC3 (13 words, call at 6944),
  ds:79EC (2 words, call at 6268), ds:7D2F (9 words, call at 5A0B),
  ds:7D95 (13 words, calls at 5CA4/5CC6; 7D9B and 7DA9 are its tails, calls at 5C04/5C33).
The executed offsets of work/cov.txt are added by p2rec.py.
SMC: offsets the program overwrites at run time (0020:65E3 writes IRET over 0020:26FA); the blocks
holding them check the byte and fall back to the interpreter when it changed.
"""
ENTRIES = [0x0FC0, 0x06F6, 0x1C4B,
           0x7C90, 0x7C8C, 0x7C2D, 0x7B91, 0x7ADF, 0x7A60, 0x78EC, 0x7898, 0x77DE, 0x773D, 0x7665, 0x760F, 0x75C4,
           0x7F6C, 0x7F26, 0x7EE2, 0x7ED8, 0x7EBF, 0x7EB5, 0x7E97, 0x7D9B, 0x7D6E, 0x7D1B,
           0x6272, 0x6277,
           0x5CDB, 0x5EC4, 0x5F30, 0x5F96, 0x5E62, 0x5E96, 0x5CCE,
           0x652C, 0x6539, 0x65AF, 0x65EF, 0x6641, 0x6657, 0x6660, 0x6669, 0x6645, 0x6672, 0x6673]
# handlers and helpers reached only through computed vectors/addresses (found as executed-shaped gaps):
# 16F9 DIET bit reader, 19C7 and 24C9 interrupt handlers, 1FCE Sound Blaster IRQ, 2391, 5F13, 66D3
ENTRIES += [0x16F9, 0x19C7, 0x1FCE, 0x2391, 0x24C9, 0x5F13, 0x66D3]
SMC = [0x26FA]
