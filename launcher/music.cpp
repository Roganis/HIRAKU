// SPDX-License-Identifier: GPL-3.0-or-later
#include "music.h"

#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#include "config.h"

namespace {

const QStringList kPatterns{"*.mid", "*.midi", "*.kar", "*.rmi"};

QString clock(double s) {
    const int t = std::max(0, (int)std::lround(s));
    return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}

QToolButton *button(QWidget *parent, const QIcon &icon, const QString &fallback, const QString &tip) {
    auto *b = new QToolButton(parent);
    b->setAutoRaise(true);
    b->setIconSize(QSize(18, 18));
    if (icon.isNull()) b->setText(fallback);
    else b->setIcon(icon);
    b->setToolTip(tip);
    return b;
}

}  // namespace

// The 16 parts' levels as an LCD's segmented meters, as the MU100 shows them: what hiraku-midi
// says ("levels" lines, 20 a second), risen to at once and falling smoothly, a peak on each
class MusicLevels : public QWidget {
public:
    MusicLevels() {
        setFixedHeight(kSegments * 4);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setToolTip("The 16 parts' levels");
        m_tick.setInterval(33);
        connect(&m_tick, &QTimer::timeout, this, [this] {
            bool any = false;
            for (int k = 0; k < 16; k++) {
                m_shown[k] = std::max(m_target[k], m_shown[k] - 0.06f);
                m_peak[k] = std::max(m_shown[k], m_peak[k] - 0.02f);
                any = any || m_peak[k] > 0;
            }
            if (!any) m_tick.stop();
            update();
        });
    }
    void set(const QString &hex) {
        for (int k = 0; k < 16 && k < hex.size(); k++) m_target[k] = QString(hex[k]).toInt(nullptr, 16) / 15.0f;
        if (!m_tick.isActive()) m_tick.start();
    }
    void clear() { set(QString(16, '0')); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const QColor lit = palette().color(QPalette::Highlight);
        QColor unlit = palette().color(QPalette::WindowText);
        unlit.setAlphaF(0.08f);
        QColor peak = palette().color(QPalette::WindowText);
        peak.setAlphaF(0.45f);
        const double w = width() / 16.0;
        for (int k = 0; k < 16; k++) {
            const int x = (int)std::lround(k * w), x2 = (int)std::lround((k + 1) * w) - 2;
            const int on = (int)std::lround(m_shown[k] * kSegments), top = (int)std::ceil(m_peak[k] * kSegments);
            for (int seg = 0; seg < kSegments; seg++) {
                const int y = height() - (seg + 1) * 4;
                p.fillRect(QRect(x, y, x2 - x, 3), seg < on ? lit : seg + 1 == top && top > on ? peak : unlit);
            }
        }
    }

private:
    static constexpr int kSegments = 7;
    float m_target[16] = {}, m_shown[16] = {}, m_peak[16] = {};
    QTimer m_tick;
};

bool MusicPlayer::isMidi(const QString &path) {
    const QString s = QFileInfo(path).suffix().toLower();
    return s == "mid" || s == "midi" || s == "kar" || s == "rmi";
}

MusicPlayer::MusicPlayer(QWidget *parent) : QWidget(parent) {
    // the song (bold), the line under it (which song of how many, the synth; or what went wrong),
    // where it is, the buttons and the volume
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 4, 8, 6);
    v->setSpacing(2);
    m_title = new QLabel;
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_title->setMinimumWidth(1);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status = new QLabel;
    m_status->setMinimumWidth(1);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QPalette grey = m_status->palette();
    grey.setColor(QPalette::WindowText, grey.color(QPalette::Disabled, QPalette::WindowText));
    m_status->setPalette(grey);
    QFont small = m_status->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    m_status->setFont(small);
    v->addWidget(m_title);
    v->addWidget(m_status);
    m_levels = new MusicLevels;
    v->addSpacing(2);
    v->addWidget(m_levels);

    auto *times = new QHBoxLayout;
    times->setSpacing(6);
    m_timeLabel = new QLabel("0:00");
    m_lengthLabel = new QLabel("0:00");
    for (QLabel *l : {m_timeLabel, m_lengthLabel}) l->setFont(small), l->setPalette(grey);
    m_position = new QSlider(Qt::Horizontal);
    m_position->setRange(0, 0);
    m_position->setToolTip("Where the song is: drag to move in it");
    times->addWidget(m_timeLabel);
    times->addWidget(m_position, 1);
    times->addWidget(m_lengthLabel);
    v->addLayout(times);

    auto *row = new QHBoxLayout;
    row->setSpacing(0);
    m_prev = button(this, style()->standardIcon(QStyle::SP_MediaSkipBackward), "|<", "Previous song (or this one again)");
    m_toggle = button(this, style()->standardIcon(QStyle::SP_MediaPlay), ">", "Play");
    m_next = button(this, style()->standardIcon(QStyle::SP_MediaSkipForward), ">|", "Next song");
    m_shuffle = button(this, QIcon::fromTheme("media-playlist-shuffle"), QString::fromUtf8("⇄"), "Shuffle");
    m_shuffle->setCheckable(true);
    m_shuffle->setChecked(cfg::settings().value("music/shuffle", true).toBool());
    m_autoplay = button(this, QIcon(), "Autoplay", "Start the music when the launcher opens");
    m_autoplay->setCheckable(true);
    m_autoplay->setFont(small);
    m_autoplay->setChecked(cfg::settings().value("music/autoplay", true).toBool());
    m_folder = button(this, style()->standardIcon(QStyle::SP_DirOpenIcon), "...", "Open the music folder");
    auto *speaker = new QLabel;
    speaker->setPixmap(style()->standardIcon(QStyle::SP_MediaVolume).pixmap(16, 16));
    m_volume = new QSlider(Qt::Horizontal);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(64);
    m_volume->setValue(cfg::settings().value("music/volume", 45).toInt());
    m_volume->setToolTip("Volume");
    row->addWidget(m_prev);
    row->addWidget(m_toggle);
    row->addWidget(m_next);
    row->addSpacing(6);
    row->addWidget(m_shuffle);
    row->addWidget(m_autoplay);
    row->addWidget(m_folder);
    row->addStretch(1);
    row->addWidget(speaker);
    row->addSpacing(4);
    row->addWidget(m_volume);
    v->addLayout(row);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);   // (the docks above take the height)

    connect(m_prev, &QToolButton::clicked, this, &MusicPlayer::previous);
    connect(m_toggle, &QToolButton::clicked, this, &MusicPlayer::toggle);
    connect(m_next, &QToolButton::clicked, this, &MusicPlayer::next);
    connect(m_folder, &QToolButton::clicked, this, &MusicPlayer::openFolder);
    connect(m_autoplay, &QToolButton::toggled, this, [](bool on) { cfg::settings().setValue("music/autoplay", on); });
    connect(m_shuffle, &QToolButton::toggled, this, [this](bool on) {
        cfg::settings().setValue("music/shuffle", on);
        const QString current = m_index >= 0 ? m_songs.value(m_index) : QString();
        scan();   // (a new order, or the folder's, from the song playing on)
        if (!current.isEmpty()) {
            m_songs.removeAll(current);
            m_songs.prepend(current);
            m_index = 0;
        }
        showSong();
    });
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        cfg::settings().setValue("music/volume", v);
        if (m_ready) send(QString("volume %1").arg(v));
    });
    // moving in the song: where the handle is let go, or where a click on the groove takes it
    auto seek = [this] {
        if (m_index < 0) return;
        const double at = m_position->value() / 10.0;
        if (m_held || m_proc.state() == QProcess::NotRunning) {
            m_at = at;
            showTimes();
            return;
        }
        const bool was = m_playing;
        play(m_index, at);
        m_playing = was;   // (a paused song stays paused there: "playing" pauses it again)
        updateButtons();
    };
    connect(m_position, &QSlider::sliderReleased, this, seek);
    connect(m_position, &QSlider::actionTriggered, this, [this, seek](int action) {
        if (action == QAbstractSlider::SliderPageStepAdd || action == QAbstractSlider::SliderPageStepSub)
            QTimer::singleShot(0, this, seek);
    });
    connect(m_position, &QSlider::sliderMoved, this, [this](int v) { m_timeLabel->setText(clock(v / 10.0)); });

    m_proc.setProcessChannelMode(QProcess::SeparateChannels);
    m_proc.setReadChannel(QProcess::StandardOutput);
    connect(&m_proc, &QProcess::readyReadStandardOutput, this, &MusicPlayer::readOutput);
    connect(&m_proc, &QProcess::finished, this, &MusicPlayer::finished);
    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        m_playing = false;
        showStatus("The music player couldn't be started", m_proc.errorString());
        updateButtons();
    });
    m_resume.setSingleShot(true);
    connect(&m_resume, &QTimer::timeout, this, [this] {
        m_held = false;
        if (m_playing && m_index >= 0) play(m_index, m_at);
        else showSong(), updateButtons();
    });

    scan();
    const int last = m_songs.indexOf(cfg::settings().value("music/last").toString());
    if (last >= 0) m_index = last;
    showSong();
    updateButtons();
}

MusicPlayer::~MusicPlayer() {
    halt();
    if (m_fetch.state() != QProcess::NotRunning) m_fetch.kill(), m_fetch.waitForFinished(1000);   // (tried again next time)
}

// The first songs: a folder of MIDI files the build names (local/music-source.txt, never in the
// repository: CMakeLists.txt's HK_MUSIC_SOURCE), or Settings' music/source, into the music folder by
// tools/musicfetch.py. On the first launch; one that can't get them all tries again on the next
// launches, three launches in all (music/fetchTries), then never again; once they are in
// (music/fetched), never again either. A build that names none gets none.
#ifndef HK_MUSIC_SOURCE
#define HK_MUSIC_SOURCE ""
#endif
void MusicPlayer::firstSongs() {
    auto &s = cfg::settings();
    const QString source = s.value("music/source", QString(HK_MUSIC_SOURCE)).toString().trimmed();
    if (source.isEmpty() || s.value("music/fetched", false).toBool()) return;
    const int tries = s.value("music/fetchTries", 0).toInt();
    if (tries >= 3) return;
    const QString script = cfg::rootDir() + "/tools/musicfetch.py";
    const QStringList py = cfg::python();
    if (py.isEmpty() || !QFileInfo::exists(script)) return;
    s.setValue("music/fetchTries", tries + 1);
    s.sync();   // (counted even if this launch ends before it does)
    m_fetch.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_fetch, &QProcess::readyReadStandardOutput, this, [this] {
        while (m_fetch.canReadLine()) {
            const QString line = QString::fromUtf8(m_fetch.readLine()).trimmed();
            if (line.startsWith("found ")) m_fetchFound = line.mid(6).toInt();
            else if (line.startsWith("got ") || line.startsWith("have ") || line.startsWith("failed ")) m_fetchDone++;
            if ((line.startsWith("got ") || line.startsWith("have ")) && m_songs.isEmpty()) {   // the first one: music
                scan();
                if (!m_playing && !m_held) autoplay();
            }
            m_fetchNote = m_fetchFound ? QString("Getting the first songs: %1 of %2").arg(m_fetchDone).arg(m_fetchFound)
                                       : QString("Getting the first songs\u2026");
            showSong();
        }
    });
    connect(&m_fetch, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        m_fetchNote.clear();
        if (st == QProcess::NormalExit && code == 0) cfg::settings().setValue("music/fetched", true);
        const QString current = m_index >= 0 ? m_songs.value(m_index) : QString();
        scan();
        if (!current.isEmpty()) m_index = m_songs.indexOf(current);
        showSong();
        updateButtons();
        if (!m_playing && !m_held && m_index < 0) autoplay();   // (the launcher opened to an empty folder)
    });
    m_fetchNote = QString("Getting the first songs\u2026");
    showSong();
    m_fetch.start(py[0], py.mid(1) + QStringList{script, source, folder()});
}

// The launcher opened: with Autoplay on, the music. Shuffled, from another song each time (the
// order is new); else from the song it was on
void MusicPlayer::autoplay() {
    if (!m_autoplay->isChecked() || m_songs.isEmpty() || m_playing) return;
    play(cfg::settings().value("music/shuffle", true).toBool() ? 0 : std::max(0, m_index));
}

void MusicPlayer::pause() {
    if (!m_playing) return;
    m_playing = false;
    if (m_ready) send("pause");
    m_levels->clear();
    updateButtons();
}

void MusicPlayer::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    showSong();
}

void MusicPlayer::watch(QProcess *p) {
    m_watched << p;
    connect(p, &QProcess::stateChanged, this, &MusicPlayer::holdChanged);
}

// A game or the game menu running: the player gone, until a moment after the last of them ends (a
// game left for the menu, or the menu for a game, is no time to start again)
void MusicPlayer::holdChanged() {
    bool any = false;
    for (const auto &p : m_watched) any = any || (p && p->state() != QProcess::NotRunning);
    if (any) {
        m_resume.stop();
        if (!m_held) {
            m_held = true;
            halt();
        }
    } else if (m_held) {
        m_resume.start(2000);
    }
    showSong();
    updateButtons();
}

void MusicPlayer::settingsChanged() {
    // the synth, its SoundFont or the folder may be others: the program again, from where it was
    const bool was = m_playing;
    const QString current = m_index >= 0 && m_index < m_songs.size() ? m_songs[m_index] : QString();
    halt();
    m_shuffle->blockSignals(true);
    m_shuffle->setChecked(cfg::settings().value("music/shuffle", true).toBool());
    m_shuffle->blockSignals(false);
    scan();
    m_index = m_songs.indexOf(current);
    m_playing = was && m_index >= 0;
    if (m_playing && !m_held) play(m_index, m_at);
    else showSong();
    updateButtons();
}

QString MusicPlayer::folder() const {
    const QString dir = cfg::settings().value("music/folder").toString().trimmed();
    return dir.isEmpty() ? cfg::musicDir() : dir;
}

void MusicPlayer::openFolder() {
    const QString dir = folder();
    if (QDir().mkpath(dir)) QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MusicPlayer::scan() {
    QStringList found;
    QDirIterator it(folder(), kPatterns, QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    while (it.hasNext()) found << it.next();
    found.sort(Qt::CaseInsensitive);
    if (cfg::settings().value("music/shuffle", true).toBool())
        std::shuffle(found.begin(), found.end(), *QRandomGenerator::global());
    // the dropped ones stay in the list, where they were
    for (const QString &s : m_songs)
        if (!found.contains(s) && QFileInfo::exists(s)) found.prepend(s);
    m_songs = found;
}

QString MusicPlayer::songName(int index) const {
    return index >= 0 && index < m_songs.size() ? QFileInfo(m_songs[index]).completeBaseName() : QString();
}

void MusicPlayer::showSong() {
    QString title, line, tip;
    if (m_index >= 0 && m_index < m_songs.size()) {
        const QString name = songName(m_index);
        title = m_songTitle.isEmpty() || m_songTitle == name ? name : name + "  ·  " + m_songTitle;
        line = QString("Song %1 of %2").arg(m_index + 1).arg(m_songs.size());
        tip = QDir::toNativeSeparators(m_songs[m_index]);
    } else if (m_songs.isEmpty()) {
        title = "No music yet";
        line = "MIDI files in the music folder, or dropped here";
        tip = QDir::toNativeSeparators(folder());
    } else {
        title = QString("%1 songs").arg(m_songs.size());
        line = "Ready";
    }
    if (m_held && m_playing) line += QString::fromUtf8(" · waits for the game");
    else if (m_playing && !m_ready && m_proc.state() != QProcess::NotRunning) line += QString::fromUtf8(" · starting…");
    else if (!m_synth.isEmpty()) line += QString::fromUtf8(" · ") + m_synth;
    if (!m_problem.isEmpty()) line = QString::fromUtf8("⚠ ") + m_problem;
    if (!m_fetchNote.isEmpty() && m_songs.isEmpty()) title = "Getting the first songs", line = m_fetchNote;
    else if (!m_fetchNote.isEmpty()) line = m_fetchNote;
    m_title->setText(m_title->fontMetrics().elidedText(title, Qt::ElideRight, m_title->width()));
    m_title->setToolTip(tip);
    m_status->setText(m_status->fontMetrics().elidedText(line, Qt::ElideRight, m_status->width()));
    m_status->setToolTip(m_problem.isEmpty() ? line : m_problem);
    showTimes();
}

void MusicPlayer::showStatus(const QString &text, const QString &tip) {
    m_problem = tip.isEmpty() ? text : text + ": " + tip;
    showSong();
}

void MusicPlayer::showTimes() {
    m_position->setEnabled(m_index >= 0 && m_length > 0);
    if (m_position->isSliderDown()) return;
    m_position->blockSignals(true);
    m_position->setRange(0, std::max(0, (int)std::lround(m_length * 10)));
    m_position->setPageStep(std::max(10, m_position->maximum() / 10));
    m_position->setValue((int)std::lround(std::clamp(m_at, 0.0, m_length) * 10));
    m_position->blockSignals(false);
    m_timeLabel->setText(clock(std::min(m_at, m_length)));
    m_lengthLabel->setText(clock(m_length));
}

void MusicPlayer::updateButtons() {
    m_toggle->setIcon(style()->standardIcon(m_playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    m_toggle->setToolTip(m_playing ? (m_held ? "Pause (the music waits for the game to end)" : "Pause") : "Play");
    m_prev->setEnabled(!m_songs.isEmpty());
    m_next->setEnabled(!m_songs.isEmpty());
}

void MusicPlayer::toggle() {
    if (m_playing) {
        pause();
        return;
    }
    if (m_songs.isEmpty()) scan();
    if (m_songs.isEmpty()) {
        QMessageBox box(QMessageBox::Information, "HIRAKU",
                        "No music yet. Put MIDI files (.mid) in the music folder, or drop them on the window:\n\n" +
                            QDir::toNativeSeparators(folder()),
                        QMessageBox::Close, this);
        QPushButton *open = box.addButton("Open the folder", QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() == open) openFolder();
        showSong();
        return;
    }
    m_playing = true;
    m_problem.clear();
    if (m_held) {
        // (it starts once the game is over)
    } else if (m_ready && m_proc.state() == QProcess::Running && m_index >= 0) {
        send("resume");
    } else {
        play(std::max(0, m_index), m_index >= 0 ? m_at : 0);
    }
    showSong();
    updateButtons();
}

// Back a song; a few seconds into one, back to its start
void MusicPlayer::previous() {
    if (m_songs.isEmpty()) return;
    const int n = m_at > 3 || m_index < 0 ? std::max(0, m_index) : (m_index + (int)m_songs.size() - 1) % (int)m_songs.size();
    m_playing = true;
    if (m_held) {
        m_index = n, m_at = 0, m_length = 0, m_songTitle.clear();
        showSong();
        updateButtons();
    } else {
        play(n);
    }
}

void MusicPlayer::next() {
    if (m_songs.isEmpty()) scan();
    if (m_songs.isEmpty()) return;
    int n = m_index + 1;
    if (n >= m_songs.size()) {   // round again: a new order when shuffled
        const QString current = m_index >= 0 ? m_songs[m_index] : QString();
        scan();
        n = 0;
        if (m_songs.size() > 1 && m_songs[0] == current) std::swap(m_songs[0], m_songs[1]);
    }
    m_playing = true;
    if (m_held) {
        m_index = n, m_at = 0, m_length = 0, m_songTitle.clear();
        showSong();
        updateButtons();
    } else {
        play(n);
    }
}

void MusicPlayer::playFiles(const QStringList &paths) {
    int at = m_index + 1;
    for (const QString &p : paths) {
        m_songs.removeAll(p);
        at = std::min<int>(at, (int)m_songs.size());
        m_songs.insert(at++, p);
    }
    const int first = at - (int)paths.size();
    m_playing = true;
    m_errors = 0;
    if (m_held) {
        m_index = first, m_at = 0, m_length = 0, m_songTitle.clear();
        showSong();
        updateButtons();
    } else {
        play(first);
    }
}

void MusicPlayer::play(int index, double at) {
    if (index < 0 || index >= m_songs.size()) return;
    if (index != m_index) m_length = 0, m_songTitle.clear();
    m_index = index;
    m_at = at;
    m_playing = true;
    m_problem.clear();
    cfg::settings().setValue("music/last", m_songs[index]);
    const QString cmd = QString("play %1 %2").arg(at, 0, 'f', 1).arg(m_songs[index]);
    if (m_proc.state() == QProcess::NotRunning) startProgram();
    if (m_ready) send(cmd);
    else m_pending = cmd;
    showSong();
    updateButtons();
}

void MusicPlayer::startProgram() {
    const QString exe = cfg::binDir() + "/" + cfg::exeName("hiraku-midi");
    if (!QFileInfo(exe).isExecutable()) {
        m_playing = false;
        showStatus("No music player", "hiraku-midi isn't in " + QDir::toNativeSeparators(cfg::binDir()));
        return;
    }
    auto &s = cfg::settings();
    QStringList a{"--synth", s.value("music/synth", "auto").toString(), "--volume", QString::number(m_volume->value())};
    // Keyboardmania's synth card: the files of any Keyboardmania set that is in
    for (const QString &id : {"kbm", "kbm2nd", "kbm3rd"})
        if (const QString rom = cfg::rootDir() + "/dumps/" + id + "/rom"; QFileInfo::exists(rom + "/xu50710-m27c160.bin")) a << "--rom" << rom;
    QString font = s.value("music/soundfont").toString().trimmed();
    if (font.isEmpty()) font = s.value("audio/soundfont").toString().trimmed();   // (Keyboardmania's)
    if (!font.isEmpty()) a << "--soundfont" << font;
#ifdef Q_OS_WIN
    // the games' output, by its name (an exclusive one too: shared, here)
    if (const QString dev = s.value("audio/device").toString(); !dev.isEmpty()) a << "--device" << dev;
#endif
    m_ready = false;
    m_buffer.clear();
    m_synth.clear();
    m_proc.start(exe, a);
}

void MusicPlayer::send(const QString &line) {
    if (m_proc.state() == QProcess::Running) m_proc.write((line + "\n").toUtf8());
}

void MusicPlayer::readOutput() {
    m_buffer += m_proc.readAllStandardOutput();
    for (int nl; (nl = m_buffer.indexOf('\n')) >= 0;) {
        const QString line = QString::fromUtf8(m_buffer.left(nl)).trimmed();
        m_buffer.remove(0, nl + 1);
        const QString word = line.section(' ', 0, 0), rest = line.section(' ', 1);
        if (word == "synth") {
            m_ready = true;
            m_synth = rest.section(' ', 1);
            if (!m_pending.isEmpty()) send(std::exchange(m_pending, QString()));
            if (!m_playing) send("pause");
            showSong();
        } else if (word == "playing") {
            m_errors = 0;
            m_problem.clear();
            m_length = rest.section(' ', 0, 0).toDouble();
            m_songTitle = rest.section(' ', 1).trimmed();
            if (!m_playing) send("pause");   // (paused while it started, or moved in while paused)
            showSong();
        } else if (word == "levels") {
            m_levels->set(rest);
        } else if (word == "at") {
            m_at = rest.toDouble();
            showTimes();
        } else if (word == "error" && !m_ready) {   // no synth at all (then it ends, code 3)
            m_playing = false;
            showStatus("No synth for the music", rest);
            updateButtons();
        } else if (word == "error") {
            m_errors++;
            showStatus("Can't play " + songName(m_index), rest.section(": ", 1));
            if (m_errors >= std::max<int>(1, (int)m_songs.size())) {   // none of them: stop asking
                m_playing = false;
                showStatus("None of the songs can be played", rest);
                updateButtons();
            }
        } else if (word == "ended") {
            if (m_playing) {
                const QString problem = m_problem;
                next();
                m_problem = problem;   // (a song skipped: said until the next one plays)
                showSong();
            }
        }
    }
}

void MusicPlayer::finished(int code, QProcess::ExitStatus) {
    m_ready = false;
    m_pending.clear();
    m_levels->clear();
    if (m_proc.property("halting").toBool()) {   // (asked to: a hold, a quit)
        m_proc.setProperty("halting", false);
        showSong();
        return;
    }
    // not asked to: no synth (code 3, said on its last line), or it stopped
    const QString why = QString::fromUtf8(m_proc.readAllStandardError()).trimmed();
    m_playing = false;
    if (code != 3) showStatus("The music player stopped", why.isEmpty() ? QString("code %1").arg(code) : why);
    updateButtons();
}

void MusicPlayer::halt() {
    m_pending.clear();
    m_levels->clear();
    if (m_proc.state() == QProcess::NotRunning) return;
    m_proc.setProperty("halting", true);
    if (!m_ready) {   // still starting (the card's start-up: about a second), nothing to hear yet: no waiting for it
        m_proc.kill();
        m_proc.waitForFinished(1000);
    } else {
        send("quit");
        m_proc.closeWriteChannel();
        if (!m_proc.waitForFinished(2000)) m_proc.kill(), m_proc.waitForFinished(1000);
    }
    m_ready = false;
}
