class Uncopyable
{
protected:
   Uncopyable()  = default;
   ~Uncopyable() = default;
private:
   Uncopyable(const Uncopyable&)            = delete;
   Uncopyable& operator=(const Uncopyable&) = delete;
   Uncopyable(Uncopyable&&)                 = delete;
   Uncopyable& operator=(Uncopyable&&)      = delete;
};