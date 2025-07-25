#pragma once

#include "JsonConfig.h"
class Busd;
class JsonConfig;

struct GlobalStruct
{
    Busd* busd_;
    JsonConfig* configMgr_;
};

GlobalStruct* GlobalSpace();