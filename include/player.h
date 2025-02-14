#ifndef __PLAYER_H__
#define __PLAYER_H__

#ifdef BOXEDWINE_RECORDER
class Player {
public:
    static bool start(BString directory);
    static Player* instance;

    void initCommandLine(BString root, const std::vector<BString>& zips, BString working, const std::vector<BString>& args);
    void runSlice();

    FILE* file;
    BString directory;
    BString version;
    U64 lastCommandTime;
    U64 lastScreenRead;
    BString nextCommand;
private:    
    BString nextValue;
    void readCommand();
};
#endif

#endif