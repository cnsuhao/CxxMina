/*
 * ENioProcessor.cpp
 *
 *  Created on: 2016-3-9
 *      Author: cxxjava@163.com
 */

#include "ENioProcessor.hh"
#include "ERuntimeIoException.hh"

namespace efc {
namespace eio {

ENioProcessor::~ENioProcessor() {
	delete selector;
}

ENioProcessor::ENioProcessor(EExecutor* executor) : EAbstractPollingIoProcessor(executor) {
	try {
		// Open a new selector
		selector = ESelector::open();
	} catch (EIOException& e) {
		EString msg("Failed to open a selector.");
		msg << "\nCause:\n  " << e.getMessage();
		throw ERuntimeIoException(__FILE__, __LINE__, msg.c_str());
	}
}

void ENioProcessor::init(sp<ENioSession>& session) {
	ESelectableChannel* ch = dynamic_cast<ESelectableChannel*>(session->getChannel());
	ch->configureBlocking(false);
	session->setSelectionKey(ch->register_(selector, ESelectionKey::OP_READ, new sp<ENioSession>(session)));
}

void ENioProcessor::destroy(sp<ENioSession>& session) {
	/* @see:
	EByteChannel* ch = session->getChannel();
	ESelectionKey* key = session->getSelectionKey();
	if (key != null) {
		key->cancel();
	}
	ch->close();
	*/

	session->destroy();
}

void ENioProcessor::doDispose() {
	selector->close();
}

int ENioProcessor::select(llong timeout) {
	return selector->select(timeout);
}

int ENioProcessor::select() {
	return selector->select();
}

boolean ENioProcessor::isSelectorEmpty() {
	return selector->keys()->isEmpty();
}

void ENioProcessor::wakeup() {
	wakeupCalled.getAndSet(true);
	selector->wakeup();
}

sp<EIterator<sp<ENioSession> > > ENioProcessor::allSessions() {
	return new IoSessionIterator(selector->keys());
}

sp<EIterator<sp<ENioSession> > > ENioProcessor::selectedSessions() {
	return new IoSessionIterator(selector->selectedKeys());
}

ESessionState ENioProcessor::getState(sp<ENioSession>& session) {
	if (session->isDestroyed()) { //added by cxxjava@163.com
		return  ESessionState::CLOSING;
	}

	ESelectionKey* key = session->getSelectionKey();

	if (key == null) {
		// The channel is not yet registred to a selector
		return ESessionState::OPENING;
	}

	if (key->isValid()) {
		// The session is opened
		return ESessionState::OPENED;
	} else {
		// The session still as to be closed
		return ESessionState::CLOSING;
	}
}

boolean ENioProcessor::isWritable(sp<ENioSession>& session) {
	ESelectionKey* key = session->getSelectionKey();

	return (key != null) && key->isValid() && key->isWritable();
}

boolean ENioProcessor::isReadable(sp<ENioSession>& session) {
	ESelectionKey* key = session->getSelectionKey();

	return (key != null) && key->isValid() && key->isReadable();
}

void ENioProcessor::setInterestedInWrite(sp<ENioSession>& session,
		boolean isInterested) {
	ESelectionKey* key = session->getSelectionKey();

	if ((key == null) || !key->isValid()) {
		return;
	}

	int newInterestOps = key->interestOps();

	if (isInterested) {
		newInterestOps |= ESelectionKey::OP_WRITE;
	} else {
		newInterestOps &= ~ESelectionKey::OP_WRITE;
	}

	key->interestOps(newInterestOps);
}

void ENioProcessor::setInterestedInRead(sp<ENioSession>& session,
		boolean isInterested) {
	ESelectionKey* key = session->getSelectionKey();

	if ((key == null) || !key->isValid()) {
		return;
	}

	int oldInterestOps = key->interestOps();
	int newInterestOps = oldInterestOps;

	if (isInterested) {
		newInterestOps |= ESelectionKey::OP_READ;
	} else {
		newInterestOps &= ~ESelectionKey::OP_READ;
	}

	if (oldInterestOps != newInterestOps) {
		key->interestOps(newInterestOps);
	}
}

boolean ENioProcessor::isInterestedInRead(sp<ENioSession>& session) {
	ESelectionKey* key = session->getSelectionKey();

	return (key != null) && key->isValid() && ((key->interestOps() & ESelectionKey::OP_READ) != 0);
}

boolean ENioProcessor::isInterestedInWrite(sp<ENioSession>& session) {
	ESelectionKey* key = session->getSelectionKey();

	return (key != null) && key->isValid() && ((key->interestOps() & ESelectionKey::OP_WRITE) != 0);
}

int ENioProcessor::read(sp<ENioSession>& session, EIoBuffer* buf) {
	EByteChannel* channel = session->getChannel();

	return channel->read(buf->buf());
}

int ENioProcessor::write(sp<ENioSession>& session, EIoBuffer* buf,
		int length) {
	if (buf->remaining() <= length) {
		return session->getChannel()->write(buf->buf());
	}

	int oldLimit = buf->limit();
	buf->limit(buf->position() + length);

	ON_FINALLY_NOTHROW (
		buf->limit(oldLimit);
	) {
		return session->getChannel()->write(buf->buf());
    }}
}

int ENioProcessor::transferFile(sp<ENioSession>& session,
		EFileRegion* region, int length) {
	/* @see:
	try {
		return (int) region.getFileChannel().transferTo(region.getPosition(), length, session.getChannel());
	} catch (IOException e) {
		// Check to see if the IOException is being thrown due to
		// http://bugs.sun.com/bugdatabase/view_bug.do?bug_id=5103988
		String message = e.getMessage();
		if ((message != null) && message.contains("temporarily unavailable")) {
			return 0;
		}

		throw e;
	}
	*/
	return (int) region->getFileChannel()->transferTo(region->getPosition(), length, session->getChannel());
}

void ENioProcessor::registerNewSelector() {
	ESelector* oldSelector = null;
	SYNCHRONIZED(selector) {
		ESet<ESelectionKey*>* keys = selector->keys();

		// Open a new selector
		ESelector* newSelector = ESelector::open();

		// Loop on all the registered keys, and register them on the new selector
		sp<EIterator<ESelectionKey*> > iter = keys->iterator();
		while (iter->hasNext()) {
			ESelectionKey* key = iter->next();
			if (!key) continue; //cxxjava@163.com
			ESelectableChannel* ch = key->channel();

			// Don't forget to attache the session, and back !
			/*@see:
			sp<ENioSession>* session = (sp<ENioSession>*)key->attachment();
			ESelectionKey* newKey = ch->register_(newSelector, key->interestOps(), session);
			(*session)->setSelectionKey(newKey);
			*/
			sp<ENioSession>* s = null;
			sp<ENioSession> session;
			SYNCHRONIZED(key) {
				s = (sp<ENioSession>*)key->attachment();
				if (s) session = (*s);
            }}
			if (s) {
				ESelectionKey* newKey = ch->register_(newSelector, key->interestOps(), s);
				session->setSelectionKey(newKey);
			}
		}

		// Now we can close the old selector and switch it
		selector->close();

		oldSelector = selector;
		selector = newSelector;
    }}
	delete oldSelector;
}

boolean ENioProcessor::isBrokenConnection() {
	// A flag set to true if we find a broken session
	boolean brokenSession = false;

	SYNCHRONIZED(selector) {
		// Get the selector keys
		ESet<ESelectionKey*>* keys = selector->keys();

		// Loop on all the keys to see if one of them
		// has a closed channel
		sp<EIterator<ESelectionKey*> > iter = keys->iterator();
		while (iter->hasNext()) {
			ESelectionKey* key = iter->next();
			ESelectableChannel* channel = key->channel();

			EDatagramChannel* dc = dynamic_cast<EDatagramChannel*>(channel);
			ESocketChannel* sc = dynamic_cast<ESocketChannel*>(channel);

			if ((dc && !dc->isConnected())
					|| (sc && !sc->isConnected())) {
				// The channel is not connected anymore. Cancel
				// the associated key then.
				key->cancel();

				// Set the flag to true to avoid a selector switch
				brokenSession = true;
			}
		}
    }}

	return brokenSession;
}

} /* namespace eio */
} /* namespace efc */
