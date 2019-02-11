/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2018 Icinga Development Team (https://icinga.com/)      *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "checker/checkercomponent.hpp"
#include "checker/checkercomponent-ti.cpp"
#include "icinga/icingaapplication.hpp"
#include "icinga/cib.hpp"
#include "remote/apilistener.hpp"
#include "base/configtype.hpp"
#include "base/defer.hpp"
#include "base/objectlock.hpp"
#include "base/utility.hpp"
#include "base/perfdatavalue.hpp"
#include "base/logger.hpp"
#include "base/exception.hpp"
#include "base/convert.hpp"
#include "base/statsfunction.hpp"

using namespace icinga;

REGISTER_TYPE(CheckerComponent);

REGISTER_STATSFUNCTION(CheckerComponent, &CheckerComponent::StatsFunc);

void CheckerComponent::StatsFunc(const Dictionary::Ptr& status, const Array::Ptr& perfdata)
{
	DictionaryData nodes;

	for (const CheckerComponent::Ptr& checker : ConfigType::GetObjectsByType<CheckerComponent>()) {
		unsigned long idle = checker->GetIdleCheckables();
		unsigned long pending = checker->GetPendingCheckables();

		nodes.emplace_back(checker->GetName(), new Dictionary({
			{ "idle", idle },
			{ "pending", pending }
		}));

		String perfdata_prefix = "checkercomponent_" + checker->GetName() + "_";
		perfdata->Add(new PerfdataValue(perfdata_prefix + "idle", Convert::ToDouble(idle)));
		perfdata->Add(new PerfdataValue(perfdata_prefix + "pending", Convert::ToDouble(pending)));
	}

	status->Set("checkercomponent", new Dictionary(std::move(nodes)));
}

void CheckerComponent::OnConfigLoaded()
{
	ConfigObject::OnActiveChanged.connect(std::bind(&CheckerComponent::ObjectHandler, this, _1));
	ConfigObject::OnPausedChanged.connect(std::bind(&CheckerComponent::ObjectHandler, this, _1));

	Checkable::OnNextCheckChanged.connect(std::bind(&CheckerComponent::NextCheckChangedHandler, this, _1));
}

void CheckerComponent::Start(bool runtimeCreated)
{
	ObjectImpl<CheckerComponent>::Start(runtimeCreated);

	Log(LogInformation, "CheckerComponent")
		<< "'" << GetName() << "' started.";

	m_RunningChecks.store(0);

	m_Thread = std::thread(std::bind(&CheckerComponent::CheckThreadProc, this));

	m_ResultTimer = new Timer();
	m_ResultTimer->SetInterval(5);
	m_ResultTimer->OnTimerExpired.connect(std::bind(&CheckerComponent::ResultTimerHandler, this));
	m_ResultTimer->Start();
}

void CheckerComponent::Stop(bool runtimeRemoved)
{
	{
		boost::mutex::scoped_lock lock(m_Mutex);
		m_Stopped = true;
		m_CV.notify_all();
	}

	m_ResultTimer->Stop();
	m_Thread.join();

	while (m_RunningChecks.load()) {
		Utility::Sleep(1.0 / 60.0);
	}

	Log(LogInformation, "CheckerComponent")
		<< "'" << GetName() << "' stopped.";

	ObjectImpl<CheckerComponent>::Stop(runtimeRemoved);
}

void CheckerComponent::CheckThreadProc()
{
	Utility::SetThreadName("Check Scheduler");

	boost::mutex::scoped_lock lock(m_Mutex);

	for (;;) {
		typedef boost::multi_index::nth_index<CheckableSet, 1>::type CheckTimeView;
		CheckTimeView& idx = boost::get<1>(m_IdleCheckables);

		while (idx.begin() == idx.end() && !m_Stopped)
			m_CV.wait(lock);

		if (m_Stopped)
			break;

		auto it = idx.begin();
		CheckableScheduleInfo csi = *it;

		double wait = csi.NextCheck - Utility::GetTime();

		if (Checkable::GetPendingChecks() >= GetConcurrentChecks())
			wait = 0.5;

		if (wait > 0) {
			/* Wait for the next check. */
			m_CV.timed_wait(lock, boost::posix_time::milliseconds(long(wait * 1000)));

			continue;
		}

		Checkable::Ptr checkable = csi.Object;

		m_IdleCheckables.erase(checkable);

		bool forced = checkable->GetForceNextCheck();
		bool check = true;

		if (!forced) {
			if (!checkable->IsReachable(DependencyCheckExecution)) {
				Log(LogNotice, "CheckerComponent")
					<< "Skipping check for object '" << checkable->GetName() << "': Dependency failed.";
				check = false;
			}

			Host::Ptr host;
			Service::Ptr service;
			tie(host, service) = GetHostService(checkable);

			if (host && !service && (!checkable->GetEnableActiveChecks() || !IcingaApplication::GetInstance()->GetEnableHostChecks())) {
				Log(LogNotice, "CheckerComponent")
					<< "Skipping check for host '" << host->GetName() << "': active host checks are disabled";
				check = false;
			}
			if (host && service && (!checkable->GetEnableActiveChecks() || !IcingaApplication::GetInstance()->GetEnableServiceChecks())) {
				Log(LogNotice, "CheckerComponent")
					<< "Skipping check for service '" << service->GetName() << "': active service checks are disabled";
				check = false;
			}

			TimePeriod::Ptr tp = checkable->GetCheckPeriod();

			if (tp && !tp->IsInside(Utility::GetTime())) {
				Log(LogNotice, "CheckerComponent")
					<< "Skipping check for object '" << checkable->GetName()
					<< "': not in check period '" << tp->GetName() << "'";
				check = false;
			}
		}

		/* reschedule the checkable if checks are disabled */
		if (!check) {
			m_IdleCheckables.insert(GetCheckableScheduleInfo(checkable));
			lock.unlock();

			Log(LogDebug, "CheckerComponent")
				<< "Checks for checkable '" << checkable->GetName() << "' are disabled. Rescheduling check.";

			checkable->UpdateNextCheck();

			lock.lock();

			continue;
		}


		csi = GetCheckableScheduleInfo(checkable);

		Log(LogDebug, "CheckerComponent")
			<< "Scheduling info for checkable '" << checkable->GetName() << "' ("
			<< Utility::FormatDateTime("%Y-%m-%d %H:%M:%S %z", checkable->GetNextCheck()) << "): Object '"
			<< csi.Object->GetName() << "', Next Check: "
			<< Utility::FormatDateTime("%Y-%m-%d %H:%M:%S %z", csi.NextCheck) << "(" << csi.NextCheck << ").";

		m_PendingCheckables.insert(csi);

		lock.unlock();

		if (forced) {
			ObjectLock olock(checkable);
			checkable->SetForceNextCheck(false);
		}

		Log(LogDebug, "CheckerComponent")
			<< "Executing check for '" << checkable->GetName() << "'";

		Checkable::IncreasePendingChecks();

		m_RunningChecks.fetch_add(1);

		Utility::QueueAsyncCallback(std::bind(&CheckerComponent::ExecuteCheckHelper, CheckerComponent::Ptr(this), checkable));

		lock.lock();
	}
}

void CheckerComponent::ExecuteCheckHelper(const Checkable::Ptr& checkable)
{
	Defer decrementRunningChecks ([this]{
		m_RunningChecks.fetch_sub(1);
	});

	try {
		checkable->ExecuteCheck();
	} catch (const std::exception& ex) {
		CheckResult::Ptr cr = new CheckResult();
		cr->SetState(ServiceUnknown);

		String output = "Exception occurred while checking '" + checkable->GetName() + "': " + DiagnosticInformation(ex);
		cr->SetOutput(output);

		double now = Utility::GetTime();
		cr->SetScheduleStart(now);
		cr->SetScheduleEnd(now);
		cr->SetExecutionStart(now);
		cr->SetExecutionEnd(now);

		checkable->ProcessCheckResult(cr);

		Log(LogCritical, "checker", output);
	}

	Checkable::DecreasePendingChecks();

	{
		boost::mutex::scoped_lock lock(m_Mutex);

		/* remove the object from the list of pending objects; if it's not in the
		 * list this was a manual (i.e. forced) check and we must not re-add the
		 * object to the list because it's already there. */
		auto it = m_PendingCheckables.find(checkable);

		if (it != m_PendingCheckables.end()) {
			m_PendingCheckables.erase(it);

			if (checkable->IsActive())
				m_IdleCheckables.insert(GetCheckableScheduleInfo(checkable));

			m_CV.notify_all();
		}
	}

	Log(LogDebug, "CheckerComponent")
		<< "Check finished for object '" << checkable->GetName() << "'";
}

void CheckerComponent::ResultTimerHandler()
{
	std::ostringstream msgbuf;

	{
		boost::mutex::scoped_lock lock(m_Mutex);

		msgbuf << "Pending checkables: " << m_PendingCheckables.size() << "; Idle checkables: " << m_IdleCheckables.size() << "; Checks/s: "
			<< (CIB::GetActiveHostChecksStatistics(60) + CIB::GetActiveServiceChecksStatistics(60)) / 60.0;
	}

	Log(LogNotice, "CheckerComponent", msgbuf.str());
}

void CheckerComponent::ObjectHandler(const ConfigObject::Ptr& object)
{
	Checkable::Ptr checkable = dynamic_pointer_cast<Checkable>(object);

	if (!checkable)
		return;

	Zone::Ptr zone = Zone::GetByName(checkable->GetZoneName());
	bool same_zone = (!zone || Zone::GetLocalZone() == zone);

	{
		boost::mutex::scoped_lock lock(m_Mutex);

		if (object->IsActive() && !object->IsPaused() && same_zone) {
			if (m_PendingCheckables.find(checkable) != m_PendingCheckables.end())
				return;

			m_IdleCheckables.insert(GetCheckableScheduleInfo(checkable));
		} else {
			m_IdleCheckables.erase(checkable);
			m_PendingCheckables.erase(checkable);
		}

		m_CV.notify_all();
	}
}

CheckableScheduleInfo CheckerComponent::GetCheckableScheduleInfo(const Checkable::Ptr& checkable)
{
	CheckableScheduleInfo csi;
	csi.Object = checkable;
	csi.NextCheck = checkable->GetNextCheck();
	return csi;
}

void CheckerComponent::NextCheckChangedHandler(const Checkable::Ptr& checkable)
{
	boost::mutex::scoped_lock lock(m_Mutex);

	/* remove and re-insert the object from the set in order to force an index update */
	typedef boost::multi_index::nth_index<CheckableSet, 0>::type CheckableView;
	CheckableView& idx = boost::get<0>(m_IdleCheckables);

	auto it = idx.find(checkable);

	if (it == idx.end())
		return;

	idx.erase(checkable);

	CheckableScheduleInfo csi = GetCheckableScheduleInfo(checkable);
	idx.insert(csi);

	m_CV.notify_all();
}

unsigned long CheckerComponent::GetIdleCheckables()
{
	boost::mutex::scoped_lock lock(m_Mutex);

	return m_IdleCheckables.size();
}

unsigned long CheckerComponent::GetPendingCheckables()
{
	boost::mutex::scoped_lock lock(m_Mutex);

	return m_PendingCheckables.size();
}
