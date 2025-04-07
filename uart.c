volatile unsigned int * const UART0DR = (unsigned int *)0x101f1000;
 
void put_uart(const char *s) {
    while(*s != '\0') {
        *UART0DR = (unsigned int)(*s);
        s++;
    }
}