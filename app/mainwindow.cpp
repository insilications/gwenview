/*
Gwenview: an image viewer
Copyright 2007 Aurélien Gâteau

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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/
#include "mainwindow.moc"

// Qt
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QListView>
#include <QTimer>
#include <QToolButton>
#include <QSplitter>

// KDE
#include <kactioncollection.h>
#include <kaction.h>
#include <kdirlister.h>
#include <kfileitem.h>
#include <klocale.h>
#include <kmimetype.h>
#include <kparts/componentfactory.h>
#include <kurl.h>
#include <kurlrequester.h>

// Local
#include <lib/thumbnailview.h>
#include <lib/mimetypeutils.h>
#include <lib/sorteddirmodel.h>

namespace Gwenview {

#undef ENABLE_LOG
#undef LOG
//#define ENABLE_LOG
#ifdef ENABLE_LOG
#define LOG(x) kDebug() << k_funcinfo << x << endl
#else
#define LOG(x) ;
#endif

struct MainWindow::Private {
	MainWindow* mWindow;
	QWidget* mDocumentView;
	QVBoxLayout* mDocumentLayout;
	QToolButton* mGoUpButton;
	KUrlRequester* mUrlRequester;
	ThumbnailView* mThumbnailView;
	QWidget* mThumbnailViewPanel;
	QFrame* mSideBar;
	KParts::ReadOnlyPart* mPart;
	QString mPartLibrary;

	QActionGroup* mViewModeActionGroup;
	QAction* mThumbsOnlyAction;
	QAction* mThumbsAndImageAction;
	QAction* mImageOnlyAction;
	QAction* mGoUpAction;

	SortedDirModel* mDirModel;

	void setupWidgets() {
		QSplitter* centralSplitter = new QSplitter(Qt::Horizontal, mWindow);
		mWindow->setCentralWidget(centralSplitter);

		QSplitter* viewSplitter = new QSplitter(Qt::Vertical, centralSplitter);
		mSideBar = new QFrame(centralSplitter);
		mSideBar->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);

		mDocumentView = new QWidget(viewSplitter);
		mDocumentLayout = new QVBoxLayout(mDocumentView);
		mDocumentLayout->setMargin(0);

		setupThumbnailView(viewSplitter);
	}

	void setupThumbnailView(QWidget* parent) {
		mThumbnailViewPanel = new QWidget(parent);

		// mThumbnailView
		mThumbnailView = new ThumbnailView(mThumbnailViewPanel);
		mThumbnailView->setModel(mDirModel);
		connect(mThumbnailView, SIGNAL(activated(const QModelIndex&)),
			mWindow, SLOT(openDirUrlFromIndex(const QModelIndex&)) );
		connect(mThumbnailView, SIGNAL(doubleClicked(const QModelIndex&)),
			mWindow, SLOT(openDirUrlFromIndex(const QModelIndex&)) );
		connect(mThumbnailView->selectionModel(), SIGNAL(currentChanged(const QModelIndex&, const QModelIndex&)),
			mWindow, SLOT(openDocumentUrlFromIndex(const QModelIndex&)) );

		// mGoUpButton
		mGoUpButton = new QToolButton(mThumbnailViewPanel);
		mGoUpButton->setAutoRaise(true);

		// mUrlRequester
		mUrlRequester = new KUrlRequester(mThumbnailViewPanel);
		mUrlRequester->setMode(KFile::Directory);
		connect(mUrlRequester, SIGNAL(urlSelected(const KUrl&)),
			mWindow, SLOT(openDirUrl(const KUrl&)) );
		connect(mUrlRequester, SIGNAL(returnPressed(const QString&)),
			mWindow, SLOT(openDirUrlFromString(const QString&)) );

		// Layout
		QGridLayout* layout = new QGridLayout(mThumbnailViewPanel);
		layout->setSpacing(0);
		layout->setMargin(0);
		layout->addWidget(mThumbnailView, 0, 0, 1, 2);
		layout->addWidget(mGoUpButton, 1, 0);
		layout->addWidget(mUrlRequester, 1, 1);
	}

	void setupActions() {
		KActionCollection* actionCollection = mWindow->actionCollection();
		mThumbsOnlyAction = actionCollection->addAction("thumbs_only");
		mThumbsOnlyAction->setText(i18n("Thumbnails"));
		mThumbsOnlyAction->setCheckable(true);

		mThumbsAndImageAction = actionCollection->addAction("thumbs_and_image");
		mThumbsAndImageAction->setText(i18n("Thumbnails and Image"));
		mThumbsAndImageAction->setCheckable(true);

		mImageOnlyAction = actionCollection->addAction("image_only");
		mImageOnlyAction->setText(i18n("Image"));
		mImageOnlyAction->setCheckable(true);

		mViewModeActionGroup = new QActionGroup(mWindow);
		mViewModeActionGroup->addAction(mThumbsOnlyAction);
		mViewModeActionGroup->addAction(mThumbsAndImageAction);
		mViewModeActionGroup->addAction(mImageOnlyAction);

		connect(mViewModeActionGroup, SIGNAL(triggered(QAction*)),
			mWindow, SLOT(setActiveViewModeAction(QAction*)) );

		mGoUpAction = actionCollection->addAction("go_up");
		mGoUpAction->setText(i18n("Go Up"));
		mGoUpAction->setIcon(KIcon("up"));
		connect(mGoUpAction, SIGNAL(triggered()),
			mWindow, SLOT(goUp()) );

		mGoUpButton->setDefaultAction(mGoUpAction);
	}

	void deletePart() {
		if (mPart) {
			//mWindow->setXMLGUIClient(0);
			delete mPart;
			mPartLibrary = QString();
		}
		mPart=0;
	}

	void createPartForUrl(const KUrl& url) {

		QString mimeType=KMimeType::findByUrl(url)->name();

		const KService::List offers = KMimeTypeTrader::self()->query( mimeType, QLatin1String("KParts/ReadOnlyPart"));
		if (offers.isEmpty()) {
			kWarning() << "Couldn't find a KPart for " << mimeType << endl;
			deletePart();
			return;
		}

		KService::Ptr service = offers.first();
		QString library=service->library();
		Q_ASSERT(!library.isNull());
		if (library == mPartLibrary) {
			LOG("Reusing current part");
			return;
		} else {
			LOG("Loading part from library: " << library);
			deletePart();
		}
		mPart = KParts::ComponentFactory::createPartInstanceFromService<KParts::ReadOnlyPart>(service, mDocumentView /*parentWidget*/, mDocumentView /*parent*/);
		if (!mPart) {
			kWarning() << "Failed to instantiate KPart from library " << library << endl;
			return;
		}
		mPartLibrary = library;
		mDocumentLayout->addWidget(mPart->widget());
		//mWindow->setXMLGUIClient(mPart);
	}
};


MainWindow::MainWindow()
: KMainWindow(0),
d(new MainWindow::Private)
{
	d->mWindow = this;
	d->mDirModel = new SortedDirModel(this);
	d->mPart = 0;
	d->setupWidgets();
	d->setupActions();
	QTimer::singleShot(0, this, SLOT(initDirModel()) );
	setupGUI();
}


void MainWindow::setActiveViewModeAction(QAction* action) {
	bool showDocument, showThumbnail;
	if (action == d->mThumbsOnlyAction) {
		showDocument = false;
		showThumbnail = true;
	} else if (action == d->mThumbsAndImageAction) {
		showDocument = true;
		showThumbnail = true;
	} else { // image only
		showDocument = true;
		showThumbnail = false;
	}

	d->mDocumentView->setVisible(showDocument);
	d->mThumbnailViewPanel->setVisible(showThumbnail);
}


void MainWindow::initDirModel() {
	KDirLister* dirLister = d->mDirModel->dirLister();
	QStringList mimeTypes;
	mimeTypes += MimeTypeUtils::dirMimeTypes();
	mimeTypes += MimeTypeUtils::imageMimeTypes();
	mimeTypes += MimeTypeUtils::videoMimeTypes();
	dirLister->setMimeFilter(mimeTypes);

	KUrl url;
	url.setPath(QDir::currentPath());
	openDirUrl(url);
}


void MainWindow::openDirUrlFromIndex(const QModelIndex& index) {
	if (!index.isValid()) {
		return;
	}

	KFileItem* item = d->mDirModel->itemForIndex(index);
	if (item->isDir()) {
		openDirUrl(item->url());
	}
}


void MainWindow::openDocumentUrlFromIndex(const QModelIndex& index) {
	if (!index.isValid()) {
		return;
	}

	KFileItem* item = d->mDirModel->itemForIndex(index);
	if (!item->isDir()) {
		openDocumentUrl(item->url());
	}
}


void MainWindow::goUp() {
	KUrl url = d->mDirModel->dirLister()->url();
	url = url.upUrl();
	openDirUrl(url);
}


void MainWindow::openDirUrl(const KUrl& url) {
	d->mDirModel->dirLister()->openUrl(url);
	d->mUrlRequester->setUrl(url);
	d->mGoUpAction->setEnabled(url.path() != "/");
}


void MainWindow::openDirUrlFromString(const QString& str) {
	KUrl url(str);
	openDirUrl(url);
}

void MainWindow::openDocumentUrl(const KUrl& url) {
	d->createPartForUrl(url);
	if (!d->mPart) return;

	d->mPart->openUrl(url);
}

} // namespace
