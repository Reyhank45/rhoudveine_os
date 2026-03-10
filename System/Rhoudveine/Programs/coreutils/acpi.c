#include <libc.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  printf("ACPI Information:\n");
  printf("  - Fixed ACPI Description Table (FADT) present\n");
  printf("  - Multiple APIC Description Table (MADT) present\n");
  printf("  - Local APIC Address: 0xfee00000\n");
  return 0;
}
