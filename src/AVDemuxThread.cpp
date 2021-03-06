/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2012-2013 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include <QtAV/AVDemuxThread.h>
#include <QtAV/AVDemuxer.h>
#include <QtAV/AVDecoder.h>
#include <QtAV/Packet.h>
#include <QtAV/AVThread.h>
#include <QtCore/QTimer>
#include <QtCore/QEventLoop>

namespace QtAV {

AVDemuxThread::AVDemuxThread(QObject *parent) :
    QThread(parent),paused(false),seeking(false),end(false)
    ,demuxer(0),audio_thread(0),video_thread(0)
{
}

AVDemuxThread::AVDemuxThread(AVDemuxer *dmx, QObject *parent) :
    QThread(parent),paused(false),seeking(false),end(false)
    ,audio_thread(0),video_thread(0)
{
    setDemuxer(dmx);
}

void AVDemuxThread::setDemuxer(AVDemuxer *dmx)
{
    demuxer = dmx;
    //TODO: will the slot not be called but the packet is token?
    //connect(dmx, SIGNAL(finished()), this, SLOT(stop()), Qt::QueuedConnection);
}

void AVDemuxThread::setAudioThread(AVThread *thread)
{
    if (audio_thread) {
        audio_thread->stop();
        delete audio_thread;
        audio_thread = 0;
    }
    audio_thread = thread;
}

void AVDemuxThread::setVideoThread(AVThread *thread)
{
    if (video_thread) {
        video_thread->stop();
        delete video_thread;
        video_thread = 0;
    }
    video_thread = thread;
}

void AVDemuxThread::seek(qreal pos)
{
    seeking = true;
    audio_thread->packetQueue()->clear();
    video_thread->packetQueue()->clear();
    demuxer->seek(pos);
    seeking = false;
    seek_cond.wakeAll();
    if (isPaused()) {
        pause(false);
        video_thread->pause(false);
        QEventLoop loop;
        QTimer::singleShot(40, &loop, SLOT(quit()));
        loop.exec();
        pause(true);
        video_thread->pause(true);
    }
}

void AVDemuxThread::seekForward()
{
    seeking = true;
    audio_thread->packetQueue()->clear();
    video_thread->packetQueue()->clear();
    demuxer->seekForward();
    seeking = false;
    seek_cond.wakeAll();
    if (isPaused()) {
        pause(false);
        video_thread->pause(false);
        QEventLoop loop;
        QTimer::singleShot(40, &loop, SLOT(quit()));
        loop.exec();
        pause(true);
        video_thread->pause(true);
    }
}

void AVDemuxThread::seekBackward()
{
    seeking = true;
    audio_thread->packetQueue()->clear();
    video_thread->packetQueue()->clear();
    demuxer->seekBackward();
    seeking = false;
    seek_cond.wakeAll();
    if (isPaused()) {
        pause(false);
        video_thread->pause(false);
        QEventLoop loop;
        QTimer::singleShot(40, &loop, SLOT(quit()));
        loop.exec();
        pause(true);
        video_thread->pause(true);
    }
}

bool AVDemuxThread::isPaused() const
{
    return paused;
}

//No more data to put. So stop blocking the queue to take the reset elements
void AVDemuxThread::stop()
{
    end = true;
    //this will not affect the pause state if we pause the output
    audio_thread->setDemuxEnded(true);
    video_thread->setDemuxEnded(true);
    audio_thread->packetQueue()->blockFull(false); //??
    video_thread->packetQueue()->blockFull(false); //?
    pause(false);
}

void AVDemuxThread::pause(bool p)
{
    if (paused == p)
        return;
    paused = p;
    if (!paused)
        cond.wakeAll();
}

void AVDemuxThread::run()
{
    end = false;
    //TODO: no video thread is ok
    Q_ASSERT(audio_thread != 0);
    Q_ASSERT(video_thread != 0);
    if (!audio_thread->isRunning())
        audio_thread->start(QThread::HighPriority);
    if (!video_thread->isRunning())
        video_thread->start();

    audio_stream = demuxer->audioStream();
    video_stream = demuxer->videoStream();
    int index = 0;
    Packet pkt;
    end = false;
    pause(false);
    PacketQueue *aqueue = audio_thread->packetQueue();
    PacketQueue *vqueue = video_thread->packetQueue();
    aqueue->setBlocking(true);
    vqueue->setBlocking(true);
    bool _has_audio = audio_thread->decoder()->isAvailable();
    bool _has_video = video_thread->decoder()->isAvailable();
    while (!end) {
        if (tryPause())
            continue; //the queue is empty and will block
        QMutexLocker locker(&buffer_mutex);
        Q_UNUSED(locker);
        if (seeking) {
            qDebug("Demuxer is seeking... wait for seek end");
            if (!seek_cond.wait(&buffer_mutex, 1000)) { //will return the same state(i.e. lock)
                qWarning("seek timed out");
            }
        }
        if (!demuxer->readFrame()) {
            continue;
        }
        index = demuxer->stream();
        pkt = *demuxer->packet(); //TODO: how to avoid additional copy?
        //connect to stop is ok too
        if (pkt.isEnd()) {
            qDebug("read end packet %d A:%d V:%d", index, audio_stream, video_stream);
            stop();
        }
        /*1 is empty but another is enough, then do not block to
          ensure the empty one can put packets immediatly.
          But usually it will not happen, why?
        */
        if (index == audio_stream) {
            if (_has_video)
                aqueue->blockFull(vqueue->size() >= vqueue->threshold());
            aqueue->put(pkt); //affect video_thread
        } else if (index == video_stream) {
            if (_has_audio)
                vqueue->blockFull(aqueue->size() >= aqueue->threshold());
            vqueue->put(pkt); //affect audio_thread
        } else { //subtitle
            continue;
        }
    }
    //flush. seeking will be omitted when stopped
    aqueue->put(Packet());
    vqueue->put(Packet());
    qDebug("Demux thread stops running....");
}

bool AVDemuxThread::tryPause()
{
    if (!paused)
        return false;
    QMutexLocker lock(&buffer_mutex);
    Q_UNUSED(lock);
    cond.wait(&buffer_mutex); //TODO: qApp->processEvents?
    return true;
}


} //namespace QtAV
