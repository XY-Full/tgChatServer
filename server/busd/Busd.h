class Busd
{
public:
    Busd() {}
    ~Busd() {}

    static Busd *instance() {
        static Busd instance;
        return &instance;
    }

    void init() {}
};