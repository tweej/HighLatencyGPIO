#include "GPIO.hh"

// STL
#include <iostream>
#include <chrono>

#include <unistd.h> // usleep()

using namespace std;
using namespace std::chrono;



std::chrono::high_resolution_clock::time_point beg;
duration<double, std::micro> accum;


class Handler
{
public:
   void handle(GPIO::Value val)
   {
      const high_resolution_clock::time_point end = high_resolution_clock::now();

      const auto time_span = end - beg;
      accum += time_span;
      cout << "Latency: " << time_span.count() << " microseconds" << endl;
   }
};


void myisr(GPIO::Value val)
{
   const high_resolution_clock::time_point end = high_resolution_clock::now();

   const auto time_span = end - beg;
   accum += time_span;
   cout << "Latency: " << time_span.count() << " microseconds" << endl;
}


int main()
{
   Handler h;

   accum = std::chrono::duration<double, std::micro>(0.0);




   // Member functions do not take any longer to call than global functions
   std::function<void(GPIO::Value)> global(myisr);

   std::function<void(GPIO::Value)> handleisr =
      std::bind(&Handler::handle, &h, std::placeholders::_1);

   {
      // Short GPIO 15 (input) to GPIO 60 (output) for the following latency test
      GPIO gpio1(60, GPIO::OUT);
      GPIO gpio2(15, GPIO::RISING, handleisr); // will be destroyed first,
                                               // so no spurious call to handleisr upon
                                               // destruction of GPIO60


      usleep(125000);

      const unsigned int nIterations = 50;
      for(unsigned int i=0;i<nIterations;++i)
      {
         beg = high_resolution_clock::now();
         gpio1.setValue(GPIO::HIGH);
         usleep(31250);

         gpio1.setValue(GPIO::LOW);
         usleep(31250);
      }

      cout << "Average: " << accum.count()/nIterations << " microseconds " << endl;
   }
}
