// library.cpp
// Created 8/23/2009 by RJ Ryan (rryan@mit.edu)

#include <QItemSelectionModel>
#include <QMessageBox>
#include <QTranslator>
#include <QDir>

#include "database/mixxxdb.h"

#include "mixer/playermanager.h"
#include "library/library.h"
#include "library/library_preferences.h"
#include "library/libraryfeature.h"
#include "library/librarytablemodel.h"
#include "library/sidebarmodel.h"
#include "library/trackcollection.h"
#include "library/trackmodel.h"
#include "library/browse/browsefeature.h"
#include "library/crate/cratefeature.h"
#include "library/rhythmbox/rhythmboxfeature.h"
#include "library/banshee/bansheefeature.h"
#include "library/recording/recordingfeature.h"
#include "library/itunes/itunesfeature.h"
#include "library/mixxxlibraryfeature.h"
#include "library/autodj/autodjfeature.h"
#include "library/playlistfeature.h"
#include "library/traktor/traktorfeature.h"
#include "library/librarycontrol.h"
#include "library/setlogfeature.h"
#include "util/db/dbconnectionpooled.h"
#include "util/sandbox.h"
#include "util/logger.h"
#include "util/assert.h"

#include "widget/wtracktableview.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"

#include "controllers/keyboard/keyboardeventfilter.h"

#include "library/externaltrackcollection.h"

namespace {

const mixxx::Logger kLogger("Library");

} // anonymous namespace

//static
const QString Library::kConfigGroup("[Library]");

//static
const ConfigKey Library::kConfigKeyRepairDatabaseOnNextRestart(kConfigGroup, "RepairDatabaseOnNextRestart");

// This is the name which we use to register the WTrackTableView with the
// WLibrary
const QString Library::m_sTrackViewName = QString("WTrackTableView");

// The default row height of the library.
const int Library::kDefaultRowHeightPx = 20;

Library::Library(
        QObject* parent,
        UserSettingsPointer pConfig,
        mixxx::DbConnectionPoolPtr pDbConnectionPool,
        PlayerManager* pPlayerManager,
        RecordingManager* pRecordingManager)
    : m_pConfig(pConfig),
      m_pDbConnectionPool(pDbConnectionPool),
      m_pSidebarModel(new SidebarModel(parent)),
      m_pTrackCollection(new TrackCollection(pConfig)),
      m_pLibraryControl(new LibraryControl(this)),
      m_pMixxxLibraryFeature(nullptr),
      m_pPlaylistFeature(nullptr),
      m_pCrateFeature(nullptr),
      m_pAnalysisFeature(nullptr),
      m_scanner(pDbConnectionPool, m_pTrackCollection, pConfig) {

    QSqlDatabase dbConnection = mixxx::DbConnectionPooled(m_pDbConnectionPool);

    // TODO(XXX): Add a checkbox in the library preferences for checking
    // and repairing the database on the next restart of the application.
    if (pConfig->getValue(kConfigKeyRepairDatabaseOnNextRestart, false)) {
        kLogger.info() << "Checking and repairing database (if necessary)";
        m_pTrackCollection->repairDatabase(dbConnection);
        // Reset config value
        pConfig->setValue(kConfigKeyRepairDatabaseOnNextRestart, false);
    }

    kLogger.info() << "Connecting database";
    m_pTrackCollection->connectDatabase(dbConnection);

#if defined(__AOIDE__)
    m_externalTrackCollections += new mixxx::aoide::TrackCollection(pConfig, m_pTrackCollection, this);
#endif

    qRegisterMetaType<Library::RemovalType>("Library::RemovalType");

    m_pKeyNotation.reset(new ControlObject(ConfigKey(kConfigGroup, "key_notation")));

    connect(&m_scanner, &LibraryScanner::scanStarted, this, &Library::scanStarted);
    connect(&m_scanner, &LibraryScanner::scanFinished, this, &Library::scanFinished);
    connect(&m_scanner, &LibraryScanner::scanFinished, this, &Library::slotRefreshLibraryModels);
    connect(&m_scanner, &LibraryScanner::trackAdded, this, &Library::slotScanTrackAdded);
    connect(&m_scanner, &LibraryScanner::tracksChanged, this, &Library::slotScanTracksUpdated);
    connect(&m_scanner, &LibraryScanner::tracksReplaced, this, &Library::slotScanTracksReplaced);

    // TODO(rryan) -- turn this construction / adding of features into a static
    // method or something -- CreateDefaultLibrary
    m_pMixxxLibraryFeature = new MixxxLibraryFeature(this, m_pTrackCollection,m_pConfig);
    addFeature(m_pMixxxLibraryFeature);

    addFeature(new AutoDJFeature(this, pConfig, pPlayerManager, m_pTrackCollection));
    m_pPlaylistFeature = new PlaylistFeature(this, m_pTrackCollection, m_pConfig);
    addFeature(m_pPlaylistFeature);
    m_pCrateFeature = new CrateFeature(this, m_pTrackCollection, m_pConfig);
    addFeature(m_pCrateFeature);
    BrowseFeature* browseFeature = new BrowseFeature(
        this, pConfig, m_pTrackCollection, pRecordingManager);
    connect(browseFeature, SIGNAL(scanLibrary()),
            &m_scanner, SLOT(scan()));
    connect(&m_scanner, SIGNAL(scanStarted()),
            browseFeature, SLOT(slotLibraryScanStarted()));
    connect(&m_scanner, SIGNAL(scanFinished()),
            browseFeature, SLOT(slotLibraryScanFinished()));

    addFeature(browseFeature);
    addFeature(new RecordingFeature(this, pConfig, m_pTrackCollection, pRecordingManager));
    addFeature(new SetlogFeature(this, pConfig, m_pTrackCollection));

    m_pAnalysisFeature = new AnalysisFeature(this, pConfig);
    connect(m_pPlaylistFeature, &PlaylistFeature::analyzeTracks,
            m_pAnalysisFeature, &AnalysisFeature::analyzeTracks);
    connect(m_pCrateFeature, &CrateFeature::analyzeTracks,
            m_pAnalysisFeature, &AnalysisFeature::analyzeTracks);
    addFeature(m_pAnalysisFeature);
    // Suspend a batch analysis while an ad-hoc analysis of
    // loaded tracks is in progress and resume it afterwards.
    connect(pPlayerManager, &PlayerManager::trackAnalyzerProgress,
            this, &Library::onPlayerManagerTrackAnalyzerProgress);
    connect(pPlayerManager, &PlayerManager::trackAnalyzerIdle,
            this, &Library::onPlayerManagerTrackAnalyzerIdle);

    //iTunes and Rhythmbox should be last until we no longer have an obnoxious
    //messagebox popup when you select them. (This forces you to reach for your
    //mouse or keyboard if you're using MIDI control and you scroll through them...)
    if (RhythmboxFeature::isSupported() &&
        pConfig->getValue(ConfigKey(kConfigGroup,"ShowRhythmboxLibrary"), true)) {
        addFeature(new RhythmboxFeature(this, m_pTrackCollection));
    }
    if (pConfig->getValue(ConfigKey(kConfigGroup,"ShowBansheeLibrary"), true)) {
        BansheeFeature::prepareDbPath(pConfig);
        if (BansheeFeature::isSupported()) {
            addFeature(new BansheeFeature(this, m_pTrackCollection, pConfig));
        }
    }
    if (ITunesFeature::isSupported() &&
        pConfig->getValue(ConfigKey(kConfigGroup,"ShowITunesLibrary"), true)) {
        addFeature(new ITunesFeature(this, m_pTrackCollection));
    }
    if (TraktorFeature::isSupported() &&
        pConfig->getValue(ConfigKey(kConfigGroup,"ShowTraktorLibrary"), true)) {
        addFeature(new TraktorFeature(this, m_pTrackCollection));
    }

    for (const auto& externalTrackCollection : m_externalTrackCollections) {
        addFeature(externalTrackCollection->newLibraryFeature(this));
    }

    // On startup we need to check if all of the user's library folders are
    // accessible to us. If the user is using a database from <1.12.0 with
    // sandboxing then we will need them to give us permission.
    QStringList directories = m_pTrackCollection->getDirectoryDAO().getDirs();

    qDebug() << "Checking for access to user's library directories:";
    foreach (QString directoryPath, directories) {
        QFileInfo directory(directoryPath);
        bool hasAccess = Sandbox::askForAccess(directory.canonicalFilePath());
        qDebug() << "Checking for access to" << directoryPath << ":" << hasAccess;
    }

    m_iTrackTableRowHeight = m_pConfig->getValue(
            ConfigKey(kConfigGroup, "RowHeight"), kDefaultRowHeightPx);
    QString fontStr = m_pConfig->getValueString(ConfigKey(kConfigGroup, "Font"));
    if (!fontStr.isEmpty()) {
        m_trackTableFont.fromString(fontStr);
    } else {
        m_trackTableFont = QApplication::font();
    }

    m_editMetadataSelectedClick = m_pConfig->getValue(
            ConfigKey(kConfigGroup, "EditMetadataSelectedClick"),
            PREF_LIBRARY_EDIT_METADATA_DEFAULT);
}

Library::~Library() {
    // Delete the sidebar model first since it depends on the LibraryFeatures.
    delete m_pSidebarModel;

    QMutableListIterator<LibraryFeature*> features_it(m_features);
    while(features_it.hasNext()) {
        LibraryFeature* feature = features_it.next();
        features_it.remove();
        delete feature;
    }

    delete m_pLibraryControl;

    for (const auto& externalTrackCollection : m_externalTrackCollections) {
        externalTrackCollection->shutdown();
    }

    kLogger.info() << "Disconnecting database";
    m_pTrackCollection->disconnectDatabase();

    //IMPORTANT: m_pTrackCollection gets destroyed via the QObject hierarchy somehow.
    //           Qt does it for us due to the way RJ wrote all this stuff.
    //Update:  - OR NOT! As of Dec 8, 2009, this pointer must be destroyed manually otherwise
    // we never see the TrackCollection's destructor being called... - Albert
    // Has to be deleted at last because the features holds references of it.
    delete m_pTrackCollection;
}

void Library::stopFeatures() {
    if (m_pAnalysisFeature) {
        m_pAnalysisFeature->stop();
        m_pAnalysisFeature = nullptr;
    }
    m_scanner.slotCancel();
}

void Library::bindSidebarWidget(WLibrarySidebar* pSidebarWidget) {
    m_pLibraryControl->bindSidebarWidget(pSidebarWidget);

    // Setup the sources view
    pSidebarWidget->setModel(m_pSidebarModel);
    connect(m_pSidebarModel, SIGNAL(selectIndex(const QModelIndex&)),
            pSidebarWidget, SLOT(selectIndex(const QModelIndex&)));
    connect(pSidebarWidget, SIGNAL(pressed(const QModelIndex&)),
            m_pSidebarModel, SLOT(pressed(const QModelIndex&)));
    connect(pSidebarWidget, SIGNAL(clicked(const QModelIndex&)),
            m_pSidebarModel, SLOT(clicked(const QModelIndex&)));
    // Lazy model: Let triangle symbol increment the model
    connect(pSidebarWidget, SIGNAL(expanded(const QModelIndex&)),
            m_pSidebarModel, SLOT(doubleClicked(const QModelIndex&)));

    connect(pSidebarWidget, SIGNAL(rightClicked(const QPoint&, const QModelIndex&)),
            m_pSidebarModel, SLOT(rightClicked(const QPoint&, const QModelIndex&)));

    pSidebarWidget->slotSetFont(m_trackTableFont);
    connect(this, SIGNAL(setTrackTableFont(QFont)),
            pSidebarWidget, SLOT(slotSetFont(QFont)));
}

void Library::bindWidget(WLibrary* pLibraryWidget,
                         KeyboardEventFilter* pKeyboard) {
    WTrackTableView* pTrackTableView =
            new WTrackTableView(
                    pLibraryWidget,
                    m_pConfig,
                    m_pTrackCollection,
                    true,
                    m_externalTrackCollections);
    pTrackTableView->installEventFilter(pKeyboard);
    connect(this, SIGNAL(showTrackModel(QAbstractItemModel*)),
            pTrackTableView, SLOT(loadTrackModel(QAbstractItemModel*)));
    connect(pTrackTableView, SIGNAL(loadTrack(TrackPointer)),
            this, SLOT(slotLoadTrack(TrackPointer)));
    connect(pTrackTableView, SIGNAL(loadTrackToPlayer(TrackPointer, QString, bool)),
            this, SLOT(slotLoadTrackToPlayer(TrackPointer, QString, bool)));
    pLibraryWidget->registerView(m_sTrackViewName, pTrackTableView);

    connect(this, SIGNAL(switchToView(const QString&)),
            pLibraryWidget, SLOT(switchToView(const QString&)));

    connect(pTrackTableView, SIGNAL(trackSelected(TrackPointer)),
            this, SIGNAL(trackSelected(TrackPointer)));

    connect(this, SIGNAL(setTrackTableFont(QFont)),
            pTrackTableView, SLOT(setTrackTableFont(QFont)));
    connect(this, SIGNAL(setTrackTableRowHeight(int)),
            pTrackTableView, SLOT(setTrackTableRowHeight(int)));
    connect(this, SIGNAL(setSelectedClick(bool)),
            pTrackTableView, SLOT(setSelectedClick(bool)));

    m_pLibraryControl->bindWidget(pLibraryWidget, pKeyboard);

    QListIterator<LibraryFeature*> feature_it(m_features);
    while(feature_it.hasNext()) {
        LibraryFeature* feature = feature_it.next();
        feature->bindWidget(pLibraryWidget, pKeyboard);
    }

    // Set the current font and row height on all the WTrackTableViews that were
    // just connected to us.
    emit(setTrackTableFont(m_trackTableFont));
    emit(setTrackTableRowHeight(m_iTrackTableRowHeight));
    emit(setSelectedClick(m_editMetadataSelectedClick));
}

void Library::addFeature(LibraryFeature* feature) {
    VERIFY_OR_DEBUG_ASSERT(feature) {
        return;
    }
    m_features.push_back(feature);
    m_pSidebarModel->addLibraryFeature(feature);
    connect(feature, SIGNAL(showTrackModel(QAbstractItemModel*)),
            this, SLOT(slotShowTrackModel(QAbstractItemModel*)));
    connect(feature, SIGNAL(switchToView(const QString&)),
            this, SLOT(slotSwitchToView(const QString&)));
    connect(feature, SIGNAL(loadTrack(TrackPointer)),
            this, SLOT(slotLoadTrack(TrackPointer)));
    connect(feature, SIGNAL(loadTrackToPlayer(TrackPointer, QString, bool)),
            this, SLOT(slotLoadTrackToPlayer(TrackPointer, QString, bool)));
    connect(feature, SIGNAL(restoreSearch(const QString&)),
            this, SLOT(slotRestoreSearch(const QString&)));
    connect(feature, SIGNAL(disableSearch()),
            this, SLOT(slotDisableSearch()));
    connect(feature, SIGNAL(enableCoverArtDisplay(bool)),
            this, SIGNAL(enableCoverArtDisplay(bool)));
    connect(feature, SIGNAL(trackSelected(TrackPointer)),
            this, SIGNAL(trackSelected(TrackPointer)));
}

void Library::onPlayerManagerTrackAnalyzerProgress(
        TrackId /*trackId*/,AnalyzerProgress /*analyzerProgress*/) {
    if (m_pAnalysisFeature) {
        m_pAnalysisFeature->suspendAnalysis();
    }
}

void Library::onPlayerManagerTrackAnalyzerIdle() {
    if (m_pAnalysisFeature) {
        m_pAnalysisFeature->resumeAnalysis();
    }
}

void Library::slotShowTrackModel(QAbstractItemModel* model) {
    //qDebug() << "Library::slotShowTrackModel" << model;
    TrackModel* trackModel = dynamic_cast<TrackModel*>(model);
    VERIFY_OR_DEBUG_ASSERT(trackModel) {
        return;
    }
    emit(showTrackModel(model));
    emit(switchToView(m_sTrackViewName));
    emit(restoreSearch(trackModel->currentSearch()));
}

void Library::slotSwitchToView(const QString& view) {
    //qDebug() << "Library::slotSwitchToView" << view;
    emit(switchToView(view));
}

void Library::slotLoadTrack(TrackPointer pTrack) {
    emit(loadTrack(pTrack));
}

void Library::slotLoadLocationToPlayer(QString location, QString group) {
    TrackPointer pTrack = m_pTrackCollection->getTrackDAO()
            .getOrAddTrack(location, true, NULL);
    if (pTrack) {
        emit(loadTrackToPlayer(pTrack, group));
    }
}

void Library::slotLoadTrackToPlayer(TrackPointer pTrack, QString group, bool play) {
    emit(loadTrackToPlayer(pTrack, group, play));
}

void Library::slotRestoreSearch(const QString& text) {
    emit restoreSearch(text);
}

void Library::slotDisableSearch() {
    emit disableSearch();
}

void Library::slotRefreshLibraryModels() {
   m_pMixxxLibraryFeature->refreshLibraryModels();
   m_pAnalysisFeature->refreshLibraryModels();
}

void Library::slotCreatePlaylist() {
    m_pPlaylistFeature->slotCreatePlaylist();
}

void Library::slotCreateCrate() {
    m_pCrateFeature->slotCreateCrate();
}

void Library::onSkinLoadFinished() {
    // Enable the default selection when a new skin is loaded.
    m_pSidebarModel->activateDefaultSelection();
}

void Library::slotRequestAddDir(QString dir) {
    // We only call this method if the user has picked a new directory via a
    // file dialog. This means the system sandboxer (if we are sandboxed) has
    // granted us permission to this folder. Create a security bookmark while we
    // have permission so that we can access the folder on future runs. We need
    // to canonicalize the path so we first wrap the directory string with a
    // QDir.
    QDir directory(dir);
    Sandbox::createSecurityToken(directory);

    if (!m_pTrackCollection->addDirectory(dir)) {
        QMessageBox::information(0, tr("Add Directory to Library"),
                tr("Could not add the directory to your library. Either this "
                    "directory is already in your library or you are currently "
                    "rescanning your library."));
    }
    // set at least one directory in the config file so that it will be possible
    // to downgrade from 1.12
    if (m_pConfig->getValueString(PREF_LEGACY_LIBRARY_DIR).length() < 1) {
        m_pConfig->set(PREF_LEGACY_LIBRARY_DIR, dir);
    }
}

void Library::slotRequestRemoveDir(QString dir, RemovalType removalType) {
    switch (removalType) {
        case Library::HideTracks:
            // Mark all tracks in this directory as deleted but DON'T purge them
            // in case the user re-adds them manually.
            m_pTrackCollection->getTrackDAO().markTracksAsMixxxDeleted(dir);
            break;
        case Library::PurgeTracks:
            // The user requested that we purge all metadata.
            purgeAllTracks(dir);
            break;
        case Library::LeaveTracksUnchanged:
        default:
            break;

    }

    // Remove the directory from the directory list.
    m_pTrackCollection->getDirectoryDAO().removeDirectory(dir);

    // Also update the config file if necessary so that downgrading is still
    // possible.
    QString confDir = m_pConfig->getValueString(PREF_LEGACY_LIBRARY_DIR);

    if (QDir(dir) == QDir(confDir)) {
        QStringList dirList = m_pTrackCollection->getDirectoryDAO().getDirs();
        if (!dirList.isEmpty()) {
            m_pConfig->set(PREF_LEGACY_LIBRARY_DIR, dirList.first());
        } else {
            // Save empty string so that an old version of mixxx knows it has to
            // ask for a new directory.
            m_pConfig->set(PREF_LEGACY_LIBRARY_DIR, QString());
        }
    }
}

void Library::slotRequestRelocateDir(QString oldDir, QString newDir) {
    relocateDirectory(oldDir, newDir);

    // also update the config file if necessary so that downgrading is still
    // possible
    QString conDir = m_pConfig->getValueString(PREF_LEGACY_LIBRARY_DIR);
    if (oldDir == conDir) {
        m_pConfig->set(PREF_LEGACY_LIBRARY_DIR, newDir);
    }
}

QStringList Library::getDirs() {
    return m_pTrackCollection->getDirectoryDAO().getDirs();
}

void Library::setFont(const QFont& font) {
    m_trackTableFont = font;
    emit(setTrackTableFont(font));
}

void Library::setRowHeight(int rowHeight) {
    m_iTrackTableRowHeight = rowHeight;
    emit(setTrackTableRowHeight(rowHeight));
}

void Library::setEditMedatataSelectedClick(bool enabled) {
    m_editMetadataSelectedClick = enabled;
    emit(setSelectedClick(enabled));
}

void Library::saveCachedTrack(Track* pTrack) noexcept {
    // It can produce dangerous signal loops if the track is still
    // sending signals while being saved!
    // See: https://bugs.launchpad.net/mixxx/+bug/1365708
    // NOTE(uklotzde, 2018-02-03): Simply disconnecting all receivers
    // doesn't seem to work reliably. Emitting the clean() signal from
    // a track that is about to deleted may cause access violations!!
    pTrack->blockSignals(true);

    // The metadata must be exported while the cache is locked to
    // ensure that we have exclusive (write) access on the file
    // and not reader or writer is accessing the same file
    // concurrently.
    m_pTrackCollection->exportTrackMetadata(pTrack);

    // The track must be saved in external track collections before
    // updating the internal database. Updating the track in the
    // internal track collection will reset its state and modify some
    // properties, e.g. clear the dirty flag. The dirty flag is used
    // to decide if the track actually needs to be sent to external
    // collections.
    if (!m_externalTrackCollections.isEmpty()) {
        if (pTrack->getId().isValid()) {
            // Track still exists in the internal collection/database
            if (pTrack->isDirty()) {
                for (const auto& externalTrackCollection : m_externalTrackCollections) {
                    externalTrackCollection->saveTrack(
                            *pTrack,
                            ExternalTrackCollection::ChangeHint::Modified);
                }
            }
        } else {
            // Track has been deleted from the local internal collection/database
            // while it was cached in-memory
            for (const auto& externalTrackCollection : m_externalTrackCollections) {
                externalTrackCollection->purgeTracks(
                        QStringList{pTrack->getLocation()});
            }
        }
    }

    // Finally update the track in the local database and reset its
    // state. This operation must be executed synchronously while
    // the cache is locked to prevent that a new track is created from
    // outdated metadata in the database before saving finished.
    m_pTrackCollection->saveTrack(pTrack);
}

void Library::relocateDirectory(QString oldDir, QString newDir) {
    for (const auto& externalTrackCollection : m_externalTrackCollections) {
        externalTrackCollection->relocateDirectory(oldDir, newDir);
    }
    m_pTrackCollection->relocateDirectory(oldDir, newDir);
}

void Library::purgeTracks(const QList<TrackId>& trackIds) {
    if (trackIds.isEmpty()) {
        return;
    }
    if (!m_externalTrackCollections.isEmpty()) {
        const auto trackLocations =
                m_pTrackCollection->getTrackDAO().getTrackLocations(trackIds);
        DEBUG_ASSERT(trackLocations.size() <= trackIds.size());
        VERIFY_OR_DEBUG_ASSERT(trackLocations.size() == trackIds.size()) {
            kLogger.warning()
                    << "Purging only"
                    << trackLocations.size()
                    << "of"
                    << trackIds.size()
                    << "tracks from external libraries";
        }
        for (const auto& externalTrackCollection : m_externalTrackCollections) {
            externalTrackCollection->purgeTracks(trackLocations);
        }
    }
    m_pTrackCollection->purgeTracks(trackIds);
}

void Library::purgeAllTracks(const QDir& rootDir) {
    for (const auto& externalTrackCollection : m_externalTrackCollections) {
        externalTrackCollection->purgeAllTracks(rootDir);
    }
    m_pTrackCollection->purgeAllTracks(rootDir);
}

void Library::slotScanTrackAdded(TrackPointer pTrack) {
    DEBUG_ASSERT(pTrack);
    for (const auto& externalTrackCollection : m_externalTrackCollections) {
        externalTrackCollection->saveTrack(*pTrack, ExternalTrackCollection::ChangeHint::Added);
    }
    // Already added to m_pTrackCollection
}

void Library::slotScanTracksUpdated(QSet<TrackId> updatedTrackIds) {
    if (updatedTrackIds.isEmpty()) {
        return;
    }
    if (!m_externalTrackCollections.isEmpty()) {
        QList<TrackRef> trackRefs;
        trackRefs.reserve(updatedTrackIds.size());
        for (const auto& trackId : updatedTrackIds) {
            auto trackLocation = m_pTrackCollection->getTrackDAO().getTrackLocation(trackId);
            if (!trackLocation.isEmpty()) {
                trackRefs.append(TrackRef::fromFileInfo(trackLocation, trackId));
            }
        }
        for (const auto& externalTrackCollection : m_externalTrackCollections) {
            externalTrackCollection->updateTracks(trackRefs);
        }
    }
    // Already updated in m_pTrackCollection
}

void Library::slotScanTracksReplaced(QList<QPair<TrackRef, TrackRef>> replacedTracks) {
    if (!m_externalTrackCollections.isEmpty()) {
        QList<ExternalTrackCollection::DuplicateTrack> duplicateTracks;
        duplicateTracks.reserve(replacedTracks.size());
        for (const auto& replacedTrack : replacedTracks) {
            ExternalTrackCollection::DuplicateTrack duplicateTrack;
            duplicateTrack.removed = replacedTrack.first;
            duplicateTrack.replacedBy = replacedTrack.second;
            duplicateTracks.append(duplicateTrack);
        }
        for (const auto& externalTrackCollection : m_externalTrackCollections) {
            externalTrackCollection->deduplicateTracks(duplicateTracks);
        }
    }
    // Already replaced in m_pTrackCollection
}
