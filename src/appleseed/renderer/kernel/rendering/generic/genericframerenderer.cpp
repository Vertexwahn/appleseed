
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010-2013 Francois Beaune, Jupiter Jazz Limited
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Interface header.
#include "genericframerenderer.h"

// appleseed.renderer headers.
#include "renderer/global/globallogger.h"
#include "renderer/kernel/rendering/generic/tilejob.h"
#include "renderer/kernel/rendering/generic/tilejobfactory.h"
#include "renderer/kernel/rendering/framerendererbase.h"
#include "renderer/kernel/rendering/ipasscallback.h"
#include "renderer/kernel/rendering/itilecallback.h"
#include "renderer/kernel/rendering/itilerenderer.h"

// appleseed.foundation headers.
#include "foundation/math/hash.h"
#include "foundation/platform/types.h"
#include "foundation/utility/foreach.h"
#include "foundation/utility/job.h"
#include "foundation/utility/statistics.h"

// Standard headers.
#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using namespace foundation;
using namespace std;

namespace renderer
{

namespace
{
    //
    // Generic frame renderer.
    //

    class GenericFrameRenderer
      : public FrameRendererBase
    {
      public:
        GenericFrameRenderer(
            const Frame&            frame,
            ITileRendererFactory*   tile_renderer_factory,
            ITileCallbackFactory*   tile_callback_factory,
            IPassCallback*          pass_callback,
            const ParamArray&       params)
          : m_frame(frame)
          , m_params(params)
          , m_pass_callback(pass_callback)
        {
            // We must have a renderer factory, but it's OK not to have a callback factory.
            assert(tile_renderer_factory);

            // Create and initialize job manager.
            m_job_manager.reset(
                new JobManager(
                    global_logger(),
                    m_job_queue,
                    m_params.m_thread_count,
                    JobManager::KeepRunningOnEmptyQueue));

            // Instantiate tile renderers, one per rendering thread.
            m_tile_renderers.reserve(m_params.m_thread_count);
            for (size_t i = 0; i < m_params.m_thread_count; ++i)
                m_tile_renderers.push_back(tile_renderer_factory->create(i == 0));

            if (tile_callback_factory)
            {
                // Instantiate tile callbacks, one per rendering thread.
                m_tile_callbacks.reserve(m_params.m_thread_count);
                for (size_t i = 0; i < m_params.m_thread_count; ++i)
                    m_tile_callbacks.push_back(tile_callback_factory->create());
            }

            print_rendering_thread_count(m_params.m_thread_count);
        }

        virtual ~GenericFrameRenderer()
        {
            // Delete tile callbacks.
            for (size_t i = 0; i < m_tile_callbacks.size(); ++i)
                m_tile_callbacks[i]->release();

            // Delete tile renderers.
            for (size_t i = 0; i < m_tile_renderers.size(); ++i)
                m_tile_renderers[i]->release();
        }

        virtual void release() OVERRIDE
        {
            delete this;
        }

        virtual void render() OVERRIDE
        {
            start_rendering();
            m_job_queue.wait_until_completion();
        }

        virtual void start_rendering() OVERRIDE
        {
            assert(!is_rendering());
            assert(!m_job_queue.has_scheduled_or_running_jobs());

            m_abort_switch.clear();

            // Create and schedule the initial (and possibly only) pass job.
            m_job_queue.schedule(
                new PassJob(
                    m_frame,
                    m_params.m_tile_ordering,
                    m_params.m_pass_count,
                    m_tile_renderers,
                    m_tile_callbacks,
                    m_pass_callback,
                    m_job_queue,
                    m_abort_switch));

            // Start job execution.
            m_job_manager->start();
        }

        virtual void stop_rendering() OVERRIDE
        {
            // Tell rendering jobs to stop.
            m_abort_switch.abort();

            // Stop job execution.
            m_job_manager->stop();

            // Delete all non-executed tile jobs.
            m_job_queue.clear_scheduled_jobs();
        }

        virtual void terminate_rendering() OVERRIDE
        {
            stop_rendering();

            print_tile_renderers_stats();
        }

        virtual bool is_rendering() const OVERRIDE
        {
            return m_job_queue.has_scheduled_or_running_jobs();
        }

      private:
        struct Parameters
        {
            const size_t                        m_thread_count;     // number of rendering threads
            const TileJobFactory::TileOrdering  m_tile_ordering;    // tile rendering order
            const size_t                        m_pass_count;       // number of rendering passes

            explicit Parameters(const ParamArray& params)
              : m_thread_count(FrameRendererBase::get_rendering_thread_count(params))
              , m_tile_ordering(get_tile_ordering(params))
              , m_pass_count(params.get_optional<size_t>("pass_count", 1))
            {
            }

            static TileJobFactory::TileOrdering get_tile_ordering(const ParamArray& params)
            {
                const string tile_ordering =
                    params.get_optional<string>("tile_ordering", "hilbert");

                if (tile_ordering == "linear")
                {
                    return TileJobFactory::LinearOrdering;
                }
                else if (tile_ordering == "spiral")
                {
                    return TileJobFactory::SpiralOrdering;
                }
                else if (tile_ordering == "hilbert")
                {
                    return TileJobFactory::HilbertOrdering;
                }
                else if (tile_ordering == "random")
                {
                    return TileJobFactory::RandomOrdering;
                }
                else
                {
                    RENDERER_LOG_ERROR(
                        "invalid value \"%s\" for parameter \"%s\", using default value \"%s\".",
                        tile_ordering.c_str(),
                        "tile_ordering",
                        "hilbert");

                    return TileJobFactory::HilbertOrdering;
                }
            }
        };

        class PassJob
          : public IJob
        {
          public:
            PassJob(
                const Frame&                        frame,
                const TileJobFactory::TileOrdering  tile_ordering,
                const size_t                        pass_count,
                vector<ITileRenderer*>&             tile_renderers,
                vector<ITileCallback*>&             tile_callbacks,
                IPassCallback*                      pass_callback,
                JobQueue&                           job_queue,
                AbortSwitch&                        abort_switch,
                const size_t                        pass = 0)
              : m_frame(frame)
              , m_tile_ordering(tile_ordering)
              , m_pass_count(pass_count)
              , m_tile_renderers(tile_renderers)
              , m_tile_callbacks(tile_callbacks)
              , m_pass_callback(pass_callback)
              , m_job_queue(job_queue)
              , m_abort_switch(abort_switch)
              , m_pass(pass)
            {
            }

            virtual void execute(const size_t thread_index) OVERRIDE
            {
                // Poor man's fence: wait until all tile jobs are done before starting a new pass.
                while (m_job_queue.get_total_job_count() != 1)
                    yield();

                // Invoke the post-pass callback (of the previous pass) if there is one.
                if (m_pass_callback && m_pass > 0)
                    m_pass_callback->post_render(m_frame);

                // Stop when all passes have been rendered.
                if (m_pass == m_pass_count)
                    return;

                // Handle aborts between passes.
                if (m_abort_switch.is_aborted())
                    return;

                // Invoke the pre-pass callback if there is one.
                if (m_pass_callback)
                    m_pass_callback->pre_render(m_frame);

                // Create tile jobs.
                const uint32 pass_hash = hashint32(static_cast<uint32>(m_pass));
                TileJobFactory::TileJobVector tile_jobs;
                m_tile_job_factory.create(
                    m_frame,
                    m_tile_ordering,
                    m_tile_renderers,
                    m_tile_callbacks,
                    pass_hash,
                    tile_jobs,
                    m_abort_switch);

                // Schedule tile jobs.
                for (const_each<TileJobFactory::TileJobVector> i = tile_jobs; i; ++i)
                    m_job_queue.schedule(*i);

                // This job reschedules itself automatically.
                m_job_queue.schedule(
                    new PassJob(
                        m_frame,
                        m_tile_ordering,
                        m_pass_count,
                        m_tile_renderers,
                        m_tile_callbacks,
                        m_pass_callback,
                        m_job_queue,
                        m_abort_switch,
                        m_pass + 1));
            }

          private:
            const Frame&                            m_frame;
            const TileJobFactory::TileOrdering      m_tile_ordering;
            const size_t                            m_pass;
            vector<ITileRenderer*>&                 m_tile_renderers;
            vector<ITileCallback*>&                 m_tile_callbacks;
            IPassCallback*                          m_pass_callback;
            const size_t                            m_pass_count;
            JobQueue&                               m_job_queue;
            AbortSwitch&                            m_abort_switch;
            TileJobFactory                          m_tile_job_factory;
        };

        const Frame&                m_frame;            // target framebuffer
        const Parameters            m_params;

        JobQueue                    m_job_queue;
        auto_ptr<JobManager>        m_job_manager;
        AbortSwitch                 m_abort_switch;

        vector<ITileRenderer*>      m_tile_renderers;   // tile renderers, one per thread
        vector<ITileCallback*>      m_tile_callbacks;   // tile callbacks, none or one per thread
        IPassCallback*              m_pass_callback;

        TileJobFactory              m_tile_job_factory;

        void print_tile_renderers_stats() const
        {
            assert(!m_tile_renderers.empty());

            StatisticsVector stats;

            for (size_t i = 0; i < m_tile_renderers.size(); ++i)
                stats.merge(m_tile_renderers[i]->get_statistics());

            RENDERER_LOG_DEBUG("%s", stats.to_string().c_str());
        }
    };
}


//
// GenericFrameRendererFactory class implementation.
//

GenericFrameRendererFactory::GenericFrameRendererFactory(
    const Frame&            frame,
    ITileRendererFactory*   tile_renderer_factory,
    ITileCallbackFactory*   tile_callback_factory,
    IPassCallback*          pass_callback,
    const ParamArray&       params)
  : m_frame(frame)
  , m_tile_renderer_factory(tile_renderer_factory)  
  , m_tile_callback_factory(tile_callback_factory)
  , m_pass_callback(pass_callback)
  , m_params(params)
{
}

void GenericFrameRendererFactory::release()
{
    delete this;
}

IFrameRenderer* GenericFrameRendererFactory::create()
{
    return
        new GenericFrameRenderer(
            m_frame,
            m_tile_renderer_factory,
            m_tile_callback_factory,
            m_pass_callback,
            m_params);
}

IFrameRenderer* GenericFrameRendererFactory::create(
    const Frame&            frame,
    ITileRendererFactory*   tile_renderer_factory,
    ITileCallbackFactory*   tile_callback_factory,
    IPassCallback*          pass_callback,
    const ParamArray&       params)
{
    return
        new GenericFrameRenderer(
            frame,
            tile_renderer_factory,
            tile_callback_factory,
            pass_callback,
            params);
}

}   // namespace renderer
