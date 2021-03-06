/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "Service.hxx"
#include "UpdateDomain.hxx"
#include "db/DatabaseListener.hxx"
#include "db/plugins/SimpleDatabasePlugin.hxx"
#include "Idle.hxx"
#include "util/Error.hxx"
#include "Log.hxx"
#include "Instance.hxx"
#include "system/FatalError.hxx"
#include "thread/Id.hxx"
#include "thread/Thread.hxx"
#include "thread/Util.hxx"

#ifndef NDEBUG
#include "event/Loop.hxx"
#endif

#include <assert.h>

inline void
UpdateService::Task()
{
	if (!next.path_utf8.empty())
		FormatDebug(update_domain, "starting: %s",
			    next.path_utf8.c_str());
	else
		LogDebug(update_domain, "starting");

	SetThreadIdlePriority();

	modified = walk.Walk(*db.GetRoot(), next.path_utf8.c_str(),
			     next.discard);

	if (modified || !db.FileExists()) {
		Error error;
		if (!db.Save(error))
			LogError(error, "Failed to save database");
	}

	if (!next.path_utf8.empty())
		FormatDebug(update_domain, "finished: %s",
			    next.path_utf8.c_str());
	else
		LogDebug(update_domain, "finished");

	progress = UPDATE_PROGRESS_DONE;
	DeferredMonitor::Schedule();
}

void
UpdateService::Task(void *ctx)
{
	UpdateService &service = *(UpdateService *)ctx;
	return service.Task();
}

void
UpdateService::StartThread(UpdateQueueItem &&i)
{
	assert(GetEventLoop().IsInsideOrNull());

	progress = UPDATE_PROGRESS_RUNNING;
	modified = false;

	next = std::move(i);

	Error error;
	if (!update_thread.Start(Task, this, error))
		FatalError(error);

	FormatDebug(update_domain,
		    "spawned thread for update job id %i", next.id);
}

unsigned
UpdateService::GenerateId()
{
	unsigned id = update_task_id + 1;
	if (id > update_task_id_max)
		id = 1;
	return id;
}

unsigned
UpdateService::Enqueue(const char *path, bool discard)
{
	assert(GetEventLoop().IsInsideOrNull());

	if (progress != UPDATE_PROGRESS_IDLE) {
		const unsigned id = GenerateId();
		if (!queue.Push(path, discard, id))
			return 0;

		update_task_id = id;
		return id;
	}

	const unsigned id = update_task_id = GenerateId();
	StartThread(UpdateQueueItem(path, discard, id));

	idle_add(IDLE_UPDATE);

	return id;
}

/**
 * Called in the main thread after the database update is finished.
 */
void
UpdateService::RunDeferred()
{
	assert(progress == UPDATE_PROGRESS_DONE);
	assert(next.IsDefined());

	update_thread.Join();
	next = UpdateQueueItem();

	idle_add(IDLE_UPDATE);

	if (modified)
		/* send "idle" events */
		listener.OnDatabaseModified();

	auto i = queue.Pop();
	if (i.IsDefined()) {
		/* schedule the next path */
		StartThread(std::move(i));
	} else {
		progress = UPDATE_PROGRESS_IDLE;
	}
}

UpdateService::UpdateService(EventLoop &_loop, SimpleDatabase &_db,
			     Storage &_storage,
			     DatabaseListener &_listener)
	:DeferredMonitor(_loop), db(_db), listener(_listener),
	 progress(UPDATE_PROGRESS_IDLE),
	 update_task_id(0),
	 walk(_loop, _listener, _storage)
{
}
