/* tntnet.cpp
 * Copyright (C) 2003-2005 Tommi Maekitalo
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "tnt/tntnet.h"
#include "tnt/worker.h"
#include "tnt/listener.h"
#include "tnt/http.h"
#include "tnt/httpreply.h"
#include "tnt/sessionscope.h"
#include "tnt/tntconfig.h"
#include "tnt/configurator.h"

#include <cxxtools/tcpstream.h>
#include <cxxtools/fork.h>
#include <cxxtools/log.h>

#include <unistd.h>

#include <config.h>

#ifndef TNTNET_CONF
# define TNTNET_CONF "/etc/tntnet.conf"
#endif

#ifndef TNTNET_PID
# define TNTNET_PID "/var/run/tntnet.pid"
#endif

log_define("tntnet.tntnet")

namespace
{
  void configureDispatcher(tnt::Dispatcher& dis, const tnt::Tntconfig& config)
  {
    typedef tnt::Dispatcher::CompidentType CompidentType;

    const tnt::Tntconfig::config_entries_type& params = config.getConfigValues();

    tnt::Tntconfig::config_entries_type::const_iterator vi;
    for (vi = params.begin(); vi != params.end(); ++vi)
    {
      const tnt::Tntconfig::config_entry_type& v = *vi;
      const tnt::Tntconfig::params_type& args = v.params;
      if (v.key == "MapUrl")
      {
        if (args.size() < 2)
        {
          std::ostringstream msg;
          msg << "invalid number of parameters (" << args.size() << ") in MapUrl";
          throw std::runtime_error(msg.str());
        }

        std::string url = args[0];

        CompidentType ci = CompidentType(args[1]);
        if (args.size() > 2)
        {
          ci.setPathInfo(args[2]);
          if (args.size() > 3)
            ci.setArgs(CompidentType::args_type(args.begin() + 3, args.end()));
        }

        dis.addUrlMapEntry(std::string(), url, ci);
      }
      else if (v.key == "VMapUrl")
      {
        if (args.size() < 3)
        {
          std::ostringstream msg;
          msg << "invalid number of parameters (" << args.size() << ") in VMapUrl";
          throw std::runtime_error(msg.str());
        }

        std::string vhost = args[0];
        std::string url = args[1];

        CompidentType ci = CompidentType(args[2]);
        if (args.size() > 3)
        {
          ci.setPathInfo(args[3]);
          if (args.size() > 4)
            ci.setArgs(CompidentType::args_type(args.begin() + 4, args.end()));
        }

        dis.addUrlMapEntry(vhost, url, ci);
      }
    }
  }

  typedef std::set<tnt::Tntnet*> TntnetInstancesType;
  cxxtools::Mutex allTntnetInstancesMutex;
  TntnetInstancesType allRunningTntnetInstances;

  // these belong actually into the Tntnet-class itself, but we don't want to
  // break binary compatibility
  cxxtools::Mutex timeStopMutex;
  cxxtools::Condition timerStopCondition;
}

namespace tnt
{
  ////////////////////////////////////////////////////////////////////////
  // Tntnet
  //
  Tntnet::Tntnet()
    : minthreads(5),
      maxthreads(100),
      threadstartdelay(10),
      timersleep(10),
      pollerthread(queue)
  { }

  bool Tntnet::stop = false;
  
  Tntnet::listeners_type Tntnet::allListeners;

  void Tntnet::init(const Tntconfig& config)
  {
    Configurator c(*this);
    c.setMinThreads(        config.getValue("MinThreads",            c.getMinThreads()));
    c.setMaxThreads(        config.getValue("MaxThreads",            c.getMaxThreads()));
    c.setThreadStartDelay(  config.getValue("ThreadStartDelay",      c.getThreadStartDelay()));
    c.setTimerSleep(        config.getValue("TimerSleep",            c.getTimerSleep()));
    c.setMaxRequestTime(    config.getValue("MaxRequestTime",        c.getMaxRequestTime()));
    c.setEnableCompression( config.getBoolValue("EnableCompression", c.getEnableCompression()));
    c.setQueueSize(         config.getValue("QueueSize",             c.getQueueSize()));
    c.setSessionTimeout(    config.getValue("SessionTimeout",        c.getSessionTimeout()));
    c.setListenBacklog(     config.getValue("ListenBacklog",         c.getListenBacklog()));
    c.setListenRetry(       config.getValue("ListenRetry",           c.getListenRetry()));
    c.setMaxUrlMapCache(    config.getValue("MaxUrlMapCache",        c.getMaxUrlMapCache()));

    Tntconfig::config_entries_type configSetEnv;
    config.getConfigValues("SetEnv", configSetEnv);
    for (Tntconfig::config_entries_type::const_iterator it = configSetEnv.begin();
         it != configSetEnv.end(); ++it)
    {
      if (it->params.size() >= 2)
      {
#ifdef HAVE_SETENV
        log_debug("setenv " << it->params[0] << "=\"" << it->params[1] << '"');
        ::setenv(it->params[0].c_str(), it->params[1].c_str(), 1);
#else
        std::string name  = it->params[0];
        std::string value = it->params[1];

        char* env = new char[name.size() + value.size() + 2];
        name.copy(env, name.size());
        env[name.size()] = '=';
        value.copy(env + name.size() + 1, value.size());
        env[name.size() + value.size() + 1] = '\0';

        log_debug("putenv(" << env << ')');
        ::putenv(env);
#endif
      }
    }

    configureDispatcher(dispatcher, config);

    // configure worker (static)
    Comploader::configure(config);

    // configure http
    c.setMaxRequestSize(    config.getValue("MaxRequestSize",      c.getMaxRequestSize()));
    c.setSocketReadTimeout( config.getValue("SocketReadTimeout",   c.getSocketReadTimeout()));
    c.setSocketWriteTimeout(config.getValue("SocketWriteTimeout",  c.getSocketWriteTimeout()));
    c.setKeepAliveMax(      config.getValue("KeepAliveMax",        c.getKeepAliveMax()));
    c.setSocketBufferSize(  config.getValue("BufferSize",          c.getSocketBufferSize()));
    c.setMinCompressSize(   config.getValue("MinCompressSize",     c.getMinCompressSize()));
    c.setKeepAliveTimeout(  config.getValue("KeepAliveTimeout",    c.getKeepAliveTimeout()));
    c.setDefaultContentType(config.getValue("DefaultContentType",  c.getDefaultContentType()));

    // initialize listeners
    Tntconfig::config_entries_type configListen;
    config.getConfigValues("Listen", configListen);

    for (Tntconfig::config_entries_type::const_iterator it = configListen.begin();
         it != configListen.end(); ++it)
    {
      if (it->params.empty())
        throw std::runtime_error("empty Listen-entry");

      unsigned short int port = 80;
      if (it->params.size() >= 2)
      {
        std::istringstream p(it->params[1]);
        p >> port;
        if (!p)
        {
          std::ostringstream msg;
          msg << "invalid port " << it->params[1];
          throw std::runtime_error(msg.str());
        }
      }

      std::string ip(it->params[0]);

      listen(ip, port);
    }

#ifdef USE_SSL
    // initialize ssl-listener
    Tntconfig::config_entries_type configSslListen;
    config.getConfigValues("SslListen", configSslListen);
    std::string defaultCertificateFile = config.getValue("SslCertificate");
    std::string defaultCertificateKey = config.getValue("SslKey");

    for (Tntconfig::config_entries_type::const_iterator it = configSslListen.begin();
         it != configSslListen.end(); ++it)
    {
      if (it->params.empty())
        throw std::runtime_error("empty SslListen-entry");

      unsigned short int port = 443;
      if (it->params.size() >= 2)
      {
        std::istringstream p(it->params[1]);
        p >> port;
        if (!p)
        {
          std::ostringstream msg;
          msg << "invalid port " << it->params[1];
          throw std::runtime_error(msg.str());
        }
      }

      std::string certificateFile =
        it->params.size() >= 3 ? it->params[2]
                               : defaultCertificateFile;
      std::string keyFile =
        it->params.size() >= 4 ? it->params[3] :
        it->params.size() >= 3 ? it->params[2] : defaultCertificateKey;

      if (certificateFile.empty())
        throw std::runtime_error("Ssl-certificate not configured");

      std::string ip(it->params[0]);

      sslListen(certificateFile, keyFile, ip, port);
    }
#endif // USE_SSL
  }

  void Tntnet::listen(const std::string& ip, unsigned short int port)
  {
    log_debug("listen on ip " << ip << " port " << port);
    ListenerBase* listener = new tnt::Listener(*this, ip, port, queue);
    listeners.insert(listener);
    allListeners.insert(listener);
  }

  void Tntnet::sslListen(const std::string& certificateFile, const std::string& keyFile, const std::string& ip, unsigned short int port)
  {
#ifdef USE_SSL
    log_debug("listen on ip " << ip << " port " << port << " (ssl)");
    ListenerBase* listener = new Ssllistener(*this, certificateFile.c_str(),
        keyFile.c_str(), ip, port, queue);
    listeners.insert(listener);
    allListeners.insert(listener);
#else
    log_error("cannot add ssl listener - ssl is not compiled into tntnet");
#endif // USE_SSL
  }

  void Tntnet::run()
  {
    log_debug("worker-process");

    stop = false;

    if (listeners.empty())
    {
      unsigned short int port = (getuid() == 0 ? 80 : 8000);
      log_info("no listeners defined - using ip 0.0.0.0 port " << port);
      listen("0.0.0.0", port);
    }
    else
      log_debug(listeners.size() << " listeners");

    if (listeners.size() >= minthreads)
    {
      log_warn("at least one more worker than listeners needed - set MinThreads to "
        << listeners.size() + 1);
      minthreads = listeners.size() + 1;
    }

    if (maxthreads < minthreads)
    {
      log_warn("MaxThreads < MinThreads - set MaxThreads = MinThreads = " << minthreads);
      maxthreads = minthreads;
    }

    // initialize worker-process
    // create worker-threads
    log_info("create " << minthreads << " worker threads");
    for (unsigned i = 0; i < minthreads; ++i)
    {
      log_debug("create worker " << i);
      Worker* s = new Worker(*this);
      s->create();
    }

    // create poller-thread
    log_debug("start poller thread");
    pollerthread.create();

    log_debug("start timer thread");
    cxxtools::MethodThread<Tntnet, cxxtools::AttachedThread> timerThread(*this, &Tntnet::timerTask);
    timerThread.create();

    {
      cxxtools::MutexLock lock(allTntnetInstancesMutex);
      allRunningTntnetInstances.insert(this);
    }

    // mainloop
    cxxtools::Mutex mutex;
    while (!stop)
    {
      {
        cxxtools::MutexLock lock(mutex);
        queue.noWaitThreads.wait(lock);
      }

      if (stop)
        break;

      if (Worker::getCountThreads() < maxthreads)
      {
        log_info("create workerthread");
        Worker* s = new Worker(*this);
        s->create();
      }
      else
        log_info("max worker-threadcount " << maxthreads << " reached");

      if (threadstartdelay > 0)
        usleep(threadstartdelay * 1000);
    }

    log_info("stopping Tntnet");

    {
      cxxtools::MutexLock lock(allTntnetInstancesMutex);
      allRunningTntnetInstances.erase(this);
    }

    log_info("stop listeners");
    while (!listeners.empty())
    {
      listeners_type::value_type s = *listeners.begin();
      log_debug("remove listener from listener-list");
      listeners.erase(s);

      log_debug("request listener to stop");
      s->doStop();

      delete s;

      log_debug("listener stopped");
    }

    log_info("stop poller thread");
    pollerthread.doStop();
    pollerthread.join();

    log_info("stop timer thread");
    timerThread.join();

    if (Worker::getCountThreads() > 0)
    {
      log_info("wait for " << Worker::getCountThreads() << " worker threads to stop");
      while (Worker::getCountThreads() > 0)
      {
        log_debug("wait for worker threads to stop; " << getQueue().getWaitThreadCount() << " left");
        usleep(100);
      }
    }

    log_info("all threads stopped");
  }

  void Tntnet::setMinThreads(unsigned n)
  {
    if (listeners.size() >= n)
    {
      log_warn("at least one more worker than listeners needed - set MinThreads to "
        << listeners.size() + 1);
      minthreads = listeners.size() + 1;
    }
    else
      minthreads = n;

  }

  void Tntnet::timerTask()
  {
    log_debug("timer thread");

    while (true)
    {
      {
        cxxtools::MutexLock timeStopLock(timeStopMutex);
        if (stop || timerStopCondition.timedwait(timeStopLock, timersleep * 1000))
          break;
      }

      getScopemanager().checkSessionTimeout();
      Worker::timer();
    }
  }

  void Tntnet::shutdown()
  {
    stop = true;

    {
      cxxtools::MutexLock timeStopLock(timeStopMutex);
      timerStopCondition.broadcast();
    }

    cxxtools::MutexLock lock(allTntnetInstancesMutex);
    for (TntnetInstancesType::iterator it = allRunningTntnetInstances.begin();
      it != allRunningTntnetInstances.end(); ++it)
    {
      (*it)->queue.noWaitThreads.signal();
      (*it)->minthreads = (*it)->maxthreads = 0;
    }
  }

  bool Tntnet::forkProcess()
  {
    cxxtools::Fork process;
    if (process.child())
    {
      // 1. child process
      while (!allListeners.empty())
      {
        listeners_type::value_type s = *allListeners.begin();
        allListeners.erase(s);
        delete s;

        log_debug("listener stopped");
      }

      cxxtools::Fork process2;
      if (process2.child())
        return true;

      exit(0);
    }

    return false;
  }

}
