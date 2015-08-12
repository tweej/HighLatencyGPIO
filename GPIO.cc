/*
The MIT License (MIT)

Copyright (c) 2014 Thomas Mercier Jr.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "GPIO.hh"

#include <fstream>
#include <stdexcept>

#include <boost/exception/diagnostic_information.hpp>
#include <boost/filesystem.hpp>

#include <sys/fcntl.h>
#include <sys/poll.h>
#include <sys/stat.h>

#include <iostream>
using std::cerr;
using std::endl;



const std::string GPIO::_sysfsPath("/sys/class/gpio/");


GPIO::GPIO(unsigned short id, Direction direction) :
   _id(id), _id_str(std::to_string(id)),
   _direction(direction),
   _edge(GPIO::Edge::NONE),
   _isr(std::function<void(Value)>()), // default constructor constructs empty function object
   _pollThread(std::thread()),         // default constructor constructs non-joinable
   _pollFD(-1),
   _isrThread(std::thread()),          // default constructor constructs non-joinable
   _destructing(false)

{
   initCommon();
}


GPIO::GPIO(unsigned short id, Edge edge, std::function<void(Value)> isr):
   _id(id), _id_str(std::to_string(id)),
   _direction(GPIO::Direction::IN),
   _edge(edge),
   _isr(isr),
   _pollThread(std::thread()), // default constructor constructs non-joinable
   _pollFD(-1),
   _isrThread(std::thread()),  // default constructor constructs non-joinable
   _destructing(false)
{
   initCommon();

   //attempt to set edge detection
   {
      std::ofstream sysfs_edge(_sysfsPath + "gpio" + _id_str + "/edge", std::ofstream::app);
      if( !sysfs_edge.is_open() )
      {
         throw std::runtime_error(
            "Unable to set edge for GPIO " + _id_str + "." +
            "Are you sure this GPIO can be configured for interrupts?");
      }
      if     ( _edge == GPIO::Edge::NONE )    sysfs_edge << "none";
      else if( _edge == GPIO::Edge::RISING )  sysfs_edge << "rising";
      else if( _edge == GPIO::Edge::FALLING ) sysfs_edge << "falling";
      else if( _edge == GPIO::Edge::BOTH )    sysfs_edge << "both";
      sysfs_edge.close();
   }


   // It is valid to use the this pointer in the constructor in this case
   // http://www.parashift.com/c++-faq/using-this-in-ctors.html
   _isrThread = std::thread(&GPIO::isrLoop, this);

   _pollThread = std::thread(&GPIO::pollLoop, this);

   sched_yield();
}


void GPIO::initCommon() const
{
   //validate id #
   {
      if( !boost::filesystem::exists(_sysfsPath) )
      {
         throw std::runtime_error(_sysfsPath + " does not exist.");
      }

      using boost::filesystem::directory_iterator;
      const directory_iterator end_itr; // default construction yields past-the-end

      bool found = false;
      for( directory_iterator itr(_sysfsPath); itr != end_itr; ++itr)
      {
         if( is_directory(itr->status()) &&
             itr->path().string().find("gpiochip") != std::string::npos )
         {
            std::ifstream infile(itr->path().string() + "/base");
            if( !infile )
            {
               throw std::runtime_error("Unable to read  " + itr->path().string() + "/base");
            }

            // There is no way to be fast and const-correct here :(
            // http://insanecoding.blogspot.com/2011/11/how-to-read-in-file-in-c.html
            std::string base;
            infile >> base;
            infile.close();

            infile.open(itr->path().string() + "/ngpio");
            if( !infile )
            {
               throw std::runtime_error("Unable to read  " + itr->path().string() + "/ngpio");
            }

            std::string ngpio;
            infile >> ngpio;
            infile.close();

            // GPIO id is valid
            if( std::stoul(base) <= _id && _id < std::stoul(base) + std::stoul(ngpio) )
            {
               found = true;
               break;
            }
         }
      }
      if( !found )
      {
         throw std::runtime_error("GPIO " + _id_str + " is invalid");
      }
   }



   // validate not already exported
   {
      // In decreasing order of speed: stat() -> access() -> fopen() -> ifstream
      struct stat stat_buf;
      const std::string path(_sysfsPath + "gpio" + _id_str);
      if( stat(path.c_str() , &stat_buf) == 0 )
      {
         throw std::runtime_error(
            "GPIO " + _id_str + " already exported." +
            "(Some other GPIO object already owns this GPIO)");
      }
   }



   // attempt to export
   {
      std::ofstream sysfs_export(_sysfsPath + "export", std::ofstream::app);
      if( !sysfs_export.is_open() )
      {
         throw std::runtime_error("Unable to export GPIO " + _id_str);
      }
      sysfs_export << _id_str;
      sysfs_export.close();
   }



   //attempt to set direction
   {
      std::ofstream sysfs_direction(
         _sysfsPath + "gpio" + _id_str + "/direction", std::ofstream::app);
      if( !sysfs_direction.is_open() )
      {
         throw std::runtime_error("Unable to set direction for GPIO " + _id_str);
      }
      if( _direction == GPIO::Direction::IN )        sysfs_direction << "in";
      else if( _direction == GPIO::Direction::OUT )  sysfs_direction << "out";
      sysfs_direction.close();
   }



   //attempt to clear active low
   {
      std::ofstream sysfs_activelow(_sysfsPath + "gpio" + _id_str + "/active_low", std::ofstream::app);
      if( !sysfs_activelow.is_open() )
      {
         throw std::runtime_error("Unable to clear active_low for GPIO " + _id_str);
      }
      sysfs_activelow << "0";
      sysfs_activelow.close();
   }



   //if output, set value to inactive
   {
      if( _direction == GPIO::Direction::OUT )
      {
         std::ofstream sysfs_value(_sysfsPath + "gpio" + _id_str + "/value", std::ofstream::app);
         if( !sysfs_value.is_open() )
         {
            throw std::runtime_error("Unable to initialize value for GPIO " + _id_str);
         }
         sysfs_value << "0";
         sysfs_value.close();
      }
   }
}


void GPIO::pollLoop()
{
   // No easy way to get file descriptor from ifstream... ugh.
   {
      const std::string path(_sysfsPath + "gpio" + _id_str + "/value");
      _pollFD = open(path.c_str(), O_RDONLY | O_NONBLOCK); // closed in destructor
      if( _pollFD < 0 )
      {
         perror("open");
         throw std::runtime_error("Unable to open " + path);
      }
   }

   // There is no way to have poll() come out of a blocking state except when it detects activity on
   // file descriptors it is monitoring, or when a process/thread blocked in poll() receives a
   // signal. Because this thread will not terminate unless poll() comes out of a blocking state at
   // some point, we need a mechanism to kick poll() out of its blocking state, even if no activity
   // is detected on the file descriptor of interest, _pollFD. I have chosen to use a pipe to
   // to acquire a stream type file descriptor which poll() will monitor for activity. I will not
   // cause any activity to occur on this file descriptor until I wish to terminate the thread.
   {
      if( pipe(_pipeFD) != 0 )
      {
         perror("pipe");
         throw std::runtime_error("Unable to create pipe");
      }
   }


   const int MAX_BUF = 2; // either 1 or 0 plus EOL
   char buf[MAX_BUF];
   struct pollfd fdset[2];

   memset((void*)fdset, 0, sizeof(fdset));

   fdset[0].fd     = _pollFD;
   fdset[0].events = POLLPRI;

   fdset[1].fd     = _pipeFD[0]; // This is the FD for the read end of the pipe
   fdset[1].events = POLLRDHUP; // Monitor for closed connection

   /// Consume the initial value
   {
      const ssize_t nbytes = read(fdset[0].fd, buf, MAX_BUF);
      if( nbytes != MAX_BUF )
      {
         // It is possible that read() could:
         //  return 1 (which could be recovered from)
         //  return 0 (which could be recovered from in the case no errors are detected)
         //  return < 0 (which could not be recovered from)
         // I suspect these cases are extraordinarily rare, and do not currently consider them to be
         // worth the amount of code necessary to gracefully recover, or the possibility of
         // introducing bugs in that code. No occurrences have been observed in over 1 year of
         // continuous operation, but I'm still willing to be wrong; just contact me if you see the
         // error below, want to make the argument that the code is necessary, or can provide said
         // code. :) This also applies to the read() in the loop below.
         if( nbytes < 0 ) perror("read1");
         throw std::runtime_error("GPIO " + _id_str + " read1() badness...");
      }
   }




   while(1)
   {
      const int rc = poll(fdset, 2, -1);
      if( rc == 1 )
      {
         if(fdset[0].revents & POLLPRI)
         {
            /// Consume the new value
            lseek(fdset[0].fd, 0, SEEK_SET);
            const ssize_t nbytes = read(fdset[0].fd, buf, MAX_BUF);
            if( nbytes != MAX_BUF ) // See comment above
            {
               if( nbytes < 0 )
                  perror("read2");

               throw std::runtime_error("GPIO " + _id_str + " read2() badness...");
            }


            Value val;
            if     ( buf[0] == '0' )  val = GPIO::Value::LOW;
            else if( buf[0] == '1' )  val = GPIO::Value::HIGH;
            else throw std::runtime_error("Invalid value read from GPIO " + _id_str + ": " + buf[0]);

   #ifdef LOCKFREE
            while( !_spsc_queue.push(val) )
               ;
   #else
            {
               std::lock_guard<std::mutex> lck(_eventMutex);
               _eventQueue.push(val);
               _eventCV.notify_one();
            }
   #endif

         }
         else // POLLRDHUP must have occurred, so end the thread
         { return; }
      }
      else if( rc < 0 )
      {
         if( errno == EINTR )
            continue;

         perror("poll");
         throw std::runtime_error("poll() error on GPIO " + _id_str);
      }
      else if( rc == 0 )
      {
         using std::runtime_error;
         throw runtime_error("poll() return code indicates timeout, which should never happen.");
      }
      else if( rc > 1 ) // POLLRDHUP must have occurred, so end the thread
      { return; }
   }
}

// Process interrupt events serially
void GPIO::isrLoop()
{
   Value val;

   while(1)
   {
#ifdef LOCKFREE
      //!***************************************** BEWARE ****************************************!/
      /// On the BeagleBone, this loop is effectively a spinlock. Unless there is a lot of GPIO
      /// activity it will be EXTREMELY wasteful of CPU time!!! (It is guaranteed to waste an entire
      /// quantum if not immediately successful at obtaining a value.) On Multicore systems this
      /// approach should provide slightly lower latency than mutexes and condition variables at the
      /// expense of CPU time. On the BeagleBone Black, it provides about 0.5 ms lower latency, but
      /// nowhere near what the BeagleBone Black PRUs can provide (nanoseconds), or even what a
      /// kernel module can provide (microseconds).
      //!*****************************************************************************************!/
      while( !_spsc_queue.pop(val) )
         if( _destructing )
            return;
#else
      std::unique_lock<std::mutex> lck(_eventMutex);
      while( _eventQueue.empty() )
      {
         _eventCV.wait(lck);

         if( _destructing == true )
            return;
      }


      val = _eventQueue.front();
      _eventQueue.pop();
      lck.unlock();
#endif

      /// *************************************************************
      /// If this (user) function causes an exception to be thrown,
      /// it will not be handled or ignored!!!
      /// *************************************************************
      _isr(val);
   }
}



GPIO::~GPIO()
{
   // Set this flag to true in order to indicate to _isrThread that it needs to terminate
   _destructing = true;
#ifndef LOCKFREE
   _eventCV.notify_one();
#endif

   // Close the file descriptors for this pipe to trigger a POLLRDHUP event, which will cause
   // _pollThread to terminate
   close(_pipeFD[0]);
   close(_pipeFD[1]);

   if( _isrThread.joinable())   _isrThread.join();
   if( _pollThread.joinable())  _pollThread.join();

   // Do not close the file descriptor for the sysfs value file until _pollThread() has joined.
   // This prevents reuse of this file descriptor by the kernel for other threads in this
   // process while the descriptor is still in use in the poll() system call.
   close(_pollFD);

   // attempt to unexport
   try
   {
      std::ofstream sysfs_unexport(_sysfsPath + "unexport", std::ofstream::app);
      if( sysfs_unexport.is_open() )
      {
         sysfs_unexport << _id_str;
         sysfs_unexport.close();
      }
      else // Do not throw exception in destructor! Effective C++ Item 8.
      {
         cerr << "Unable to unexport GPIO " + _id_str + "!" << endl;
         cerr << "This will prevent initialization of another GPIO object for this GPIO." << endl;
      }
   }
   catch(...)
   {
      cerr << "Exception caught in destructor for GPIO " << _id_str << endl;
      cerr << boost::current_exception_diagnostic_information() << endl;
   }
}


void GPIO::setValue(const Value value) const
{
   if( _direction == GPIO::Direction::IN )
   {
      throw std::runtime_error("Cannot set value on an input GPIO");
   }

   std::ofstream sysfs_value(_sysfsPath + "gpio" + _id_str + "/value", std::ofstream::app);
   if( !sysfs_value.is_open() )
   {
      throw std::runtime_error("Unable to set value for GPIO " + _id_str);
   }

   if     ( value == GPIO::Value::HIGH )  sysfs_value << "1";
   else if( value == GPIO::Value::LOW )   sysfs_value << "0";
   sysfs_value.close();
}


GPIO::Value GPIO::getValue() const
{
   std::ifstream sysfs_value(_sysfsPath + "gpio" + _id_str + "/value");
   if( !sysfs_value.is_open() )
   {
      throw std::runtime_error("Unable to get value for GPIO " + _id_str);
   }

   const char value = sysfs_value.get();
   if( !sysfs_value.good() )
   {
      throw std::runtime_error("Unable to get value for GPIO " + _id_str);
   }

   Value val;
   if     ( value == '0' )  val = GPIO::Value::LOW;
   else if( value == '1' )  val = GPIO::Value::HIGH;
   else throw std::runtime_error("Invalid value read from GPIO " + _id_str + ": " + value);

   return(val);
}
