// vim: set tabstop=4 shiftwidth=4 noexpandtab
/*
Gwenview - A simple image viewer for KDE
Copyright 2000-2004 Aur�lien G�teau
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 
*/

// Qt
#include <qbuffer.h>
#include <qfile.h>
#include <qguardedptr.h>
#include <qimage.h>
#include <qmemarray.h>
#include <qstring.h>
#include <qtimer.h>

// KDE
#include <kdebug.h>
#include <kio/job.h>
#include <kio/netaccess.h>
#include <ktempfile.h>
#include <kurl.h>

// Local
#include "gvdocumentloadedimpl.h"
#include "gvdocumentjpegloadedimpl.h"
#include "gvdocumentdecodeimpl.moc"


const unsigned int DECODE_CHUNK_SIZE=4096;


//---------------------------------------------------------------------
//
// GVDocumentDecodeImplPrivate
//
//---------------------------------------------------------------------
class GVDocumentDecodeImplPrivate {
public:
	GVDocumentDecodeImplPrivate(GVDocumentDecodeImpl* impl)
	: mReadSize( 0 ), mDecoder(impl), mSuspended( false ) {}

	bool mUpdatedDuringLoad;
	QByteArray mRawData;
	unsigned int mReadSize;
	QImageDecoder mDecoder;
	QTimer mDecoderTimer;
	QRect mLoadChangedRect;
	QTime mLoadCompressChangesTime;
	QGuardedPtr<KIO::Job> mJob;
	bool mSuspended;
	bool mSyncDecode;
};


//---------------------------------------------------------------------
//
// GVDocumentDecodeImpl
//
//---------------------------------------------------------------------
GVDocumentDecodeImpl::GVDocumentDecodeImpl(GVDocument* document) 
: GVDocumentImpl(document) {
	kdDebug() << k_funcinfo << endl;
	d=new GVDocumentDecodeImplPrivate(this);
	d->mUpdatedDuringLoad=false;

	connect(&d->mDecoderTimer, SIGNAL(timeout()), this, SLOT(loadChunk()) );
	
	QTimer::singleShot(0, this, SLOT(startLoading()) );
}


GVDocumentDecodeImpl::~GVDocumentDecodeImpl() {
	delete d;
}

void GVDocumentDecodeImpl::startLoading() {
	d->mJob=KIO::get(mDocument->url(), false, false);

	connect(d->mJob, SIGNAL(data(KIO::Job*, const QByteArray&)),
		this, SLOT(slotDataReceived(KIO::Job*, const QByteArray&)) );
	
	connect(d->mJob, SIGNAL(result(KIO::Job*)),
		this, SLOT(slotResult(KIO::Job*)) );

	d->mRawData.resize(0);
	d->mReadSize=0;
	d->mLoadChangedRect=QRect();
	d->mSyncDecode = false;
	d->mLoadCompressChangesTime.start();
}
    

void GVDocumentDecodeImpl::slotResult(KIO::Job* job) {
	kdDebug() << k_funcinfo << " loading finished:" << job->error() << endl;
	if( job->error() == 0 ) {
		d->mDecoderTimer.start(0, false);
		return;
	}
	// failed
	emit finished(false);
	switchToImpl(new GVDocumentImpl(mDocument));
}


void GVDocumentDecodeImpl::slotDataReceived(KIO::Job*, const QByteArray& chunk) {
	if (chunk.size()>0) {
		int oldSize=d->mRawData.size();
		d->mRawData.resize(oldSize + chunk.size());
		memcpy(d->mRawData.data()+oldSize, chunk.data(), chunk.size() );
	}
	// Decode the the image with a timer
	if( !d->mDecoderTimer.isActive()) d->mDecoderTimer.start(0, false);
}


QImage GVDocumentDecodeImpl::asyncDecode(bool& ok) {
	ok = true;
	int decodedSize=0;
	int chunkSize = QMIN(DECODE_CHUNK_SIZE, int(d->mRawData.size())-d->mReadSize);
	if( chunkSize > 0 ) {
		decodedSize = d->mDecoder.decode(
			(const uchar*)(d->mRawData.data()+d->mReadSize),
			chunkSize);
	}
	// There's more to load
	if (decodedSize>0) {
		d->mReadSize+=decodedSize;
		return QImage();
	}

	// We decoded all the available data, but the job is still running
	if (decodedSize==0 && !d->mJob.isNull()) {
		return QImage();
	}
	// If async loading failed, try synchronous loading
	if( decodedSize < 0 ) {
		kdDebug() << k_funcinfo << " async loading failed, trying sync loading\n";
		d->mSyncDecode = true;
		return syncDecode( ok );
	}
	return d->mDecoder.image();
}

QImage GVDocumentDecodeImpl::syncDecode(bool& ok) {
	ok = true;
	if( !d->mJob.isNull()) return QImage(); // wait for whole image
	d->mUpdatedDuringLoad=false;
	QImage image;
	d->mReadSize = d->mRawData.size();
	if( !image.loadFromData(d->mRawData)) {
		ok = false;
		return QImage();
	}
	return image;
}

void GVDocumentDecodeImpl::loadChunk() {
	if( d->mSuspended ) {
		d->mDecoderTimer.stop();
	        return;
	}

	bool ok;
	QImage image = d->mSyncDecode ? syncDecode(ok) : asyncDecode(ok);
	
	// Image can't be loaded, let's switch to an empty implementation
	if (!ok) {
		kdDebug() << k_funcinfo << " loading failed\n";
		d->mDecoderTimer.stop();
		emit finished(false);
		switchToImpl(new GVDocumentImpl(mDocument));
		return;
	}
	if( image.isNull()) { // not completely loaded yet
		if(d->mReadSize == d->mRawData.size() && !d->mJob.isNull()) {
			d->mDecoderTimer.stop(); // wait for more data
		}
		return;
	}

	kdDebug() << k_funcinfo << " loading succeded\n";
	d->mDecoderTimer.stop();

	// Set the image format. QImageIO::imageFormat should not fail since at
	// this point the image has been decoded successfully.
	QBuffer buffer(d->mRawData);
	buffer.open(IO_ReadOnly);
	setImageFormat( QImageIO::imageFormat(&buffer) );
	Q_ASSERT(mDocument->imageFormat()!=0);
	buffer.close();

	// Convert depth if necessary
	// (32 bit depth is necessary for alpha-blending)
	if (image.depth()<32 && image.hasAlphaBuffer()) {
		image=image.convertDepth(32);
		d->mUpdatedDuringLoad=false;
	}

	// The decoder did not cause the sizeUpdated or rectUpdated signals to be
	// emitted, let's do it now
	if (!d->mUpdatedDuringLoad) {
		setImage(image);
		emit sizeUpdated(image.width(), image.height());
		emit rectUpdated( QRect(QPoint(0,0), image.size()) );
	}
	
	// Now we switch to a loaded implementation
	if (qstrcmp(mDocument->imageFormat(), "JPEG")==0) {
		// We want a local copy of the file for the comment editor
		QString tempFilePath;
		if (!mDocument->url().isLocalFile()) {
			KTempFile tempFile(QString("gvremotefile"));
			tempFile.dataStream()->writeRawBytes(d->mRawData.data(), d->mRawData.size());
			tempFile.close();
			tempFilePath=tempFile.name();
		}
		switchToImpl(new GVDocumentJPEGLoadedImpl(mDocument, d->mRawData, tempFilePath));
	} else {
		switchToImpl(new GVDocumentLoadedImpl(mDocument));
	}
}


void GVDocumentDecodeImpl::suspendLoading() {
	d->mDecoderTimer.stop();
	d->mSuspended = true;
}

void GVDocumentDecodeImpl::resumeLoading() {
	d->mSuspended = false;
	if(d->mReadSize < d->mRawData.size() || d->mJob.isNull()) {
		d->mDecoderTimer.start(0, false);
	}
}


//---------------------------------------------------------------------
//
// QImageConsumer
//
//---------------------------------------------------------------------
void GVDocumentDecodeImpl::end() {
	if( !d->mLoadChangedRect.isNull()) {
		emit rectUpdated(d->mLoadChangedRect);
	}
	kdDebug() << k_funcinfo << endl;
}

void GVDocumentDecodeImpl::changed(const QRect& rect) {
//	kdDebug() << k_funcinfo << " " << rect.left() << "-" << rect.top() << " " << rect.width() << "x" << rect.height() << endl;
	if (!d->mUpdatedDuringLoad) {
		setImage(d->mDecoder.image());
		d->mUpdatedDuringLoad=true;
	}
	d->mLoadChangedRect |= rect;
	if( d->mLoadCompressChangesTime.elapsed() > 100 ) {
		kdDebug() << k_funcinfo << " " << d->mLoadChangedRect.left() << "-" << d->mLoadChangedRect.top()
			<< " " << d->mLoadChangedRect.width() << "x" << d->mLoadChangedRect.height() << "\n";
		emit rectUpdated(d->mLoadChangedRect);
		d->mLoadChangedRect = QRect();
		d->mLoadCompressChangesTime.start();
	}
}

void GVDocumentDecodeImpl::frameDone() {
	kdDebug() << k_funcinfo << endl;
}

void GVDocumentDecodeImpl::frameDone(const QPoint& /*offset*/, const QRect& /*rect*/) {
	kdDebug() << k_funcinfo << endl;
}

void GVDocumentDecodeImpl::setLooping(int) {
	kdDebug() << k_funcinfo << endl;
}

void GVDocumentDecodeImpl::setFramePeriod(int /*milliseconds*/) {
	kdDebug() << k_funcinfo << endl;
}

void GVDocumentDecodeImpl::setSize(int width, int height) {
	kdDebug() << k_funcinfo << " " << width << "x" << height << endl;
	// FIXME: There must be a better way than creating an empty image
	setImage(QImage(width, height, 32));
	emit sizeUpdated(width, height);
}

