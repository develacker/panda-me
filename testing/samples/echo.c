#include <stdio.h>

int main(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("%s [arg1]\n", argv[0]);
        return -1;
    }

    printf("=============== ECHO SERVICE ================\n");
    printf("%s\n", argv[1]);
    printf("=============================================\n");
    return 0;
}

