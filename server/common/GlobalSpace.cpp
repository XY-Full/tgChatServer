#include "GlobalSpace.h"

GlobalStruct* GlobalSpace()
{
    static GlobalStruct globalSpace;
    return &globalSpace;
}