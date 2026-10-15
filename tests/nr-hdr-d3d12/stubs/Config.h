#pragma once
struct Config
{
    struct Precompiled
    {
        bool value_or_default() const { return true; }
    } UsePrecompiledShaders;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
