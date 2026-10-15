#pragma once
struct State
{
    bool isShuttingDown = false;
    static State& Instance()
    {
        static State state;
        return state;
    }
};

struct ScopedSkipHeapCapture
{
};
