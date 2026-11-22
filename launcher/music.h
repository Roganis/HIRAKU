// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's music: MIDI files played while it is open, by hiraku-midi (runtime/midi/player.cpp)
// through Keyboardmania's synth card, a SoundFont or Windows' own synth. A small panel, the Music
// dock under the details and the log (View > Music): the song, the 16 parts' level meters, where it
// is (drag to move in it), previous / play-pause / next, shuffle, autoplay, the folder, the volume.
//
// The songs: the music folder (Settings > Audio; by default <saves>/music, made when first asked
// for) and what is dropped on the window. The player stops while a game or the game menu runs
// (they want the sound device and the CPU) and goes on where it was a moment after.
#pragma once

#include <QList>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class MusicLevels;
class QLabel;
class QResizeEvent;
class QSlider;
class QToolButton;

class MusicPlayer : public QWidget {
public:
    explicit MusicPlayer(QWidget *parent = nullptr);
    ~MusicPlayer() override;

    static bool isMidi(const QString &path);   // .mid, .midi, .kar, .rmi
    void watch(QProcess *p);                   // held while it runs (a game, the game menu)
    void playFiles(const QStringList &paths);  // dropped: the first now, the others next
    void settingsChanged();                    // Settings > Audio: the synth, its SoundFont, the folder
    void autoplay();                           // the launcher opened: the music, when Autoplay is on
    void firstSongs();                         // ... and on its first launches, the first songs (music.cpp)
    void pause();                              // (the panel closed: no music nobody can stop)
    void themeChanged();                       // View > Look (theme.h): its icons again
    void arrange(bool bar);                    // a panel, or a bar along the window's bottom (the cards view)

protected:
    void resizeEvent(QResizeEvent *e) override;   // (the title elided to the width)

private:
    void toggle();
    void previous();
    void next();
    void play(int index, double at = 0);
    void halt();                         // the program ends (a hold, a quit); what was playing is kept
    void startProgram();
    void readOutput();
    void finished(int code, QProcess::ExitStatus st);
    void holdChanged();
    void scan();                         // the folder's songs (a new order when shuffled)
    void openFolder();
    QString folder() const;
    void showSong();                     // the title, the line under it, the times
    void fitBar();                       // the bar to the window's width: less in it on a small screen
    void showStatus(const QString &text, const QString &tip = QString());   // a problem, under the title
    void showTimes();
    void updateButtons();
    void showSpeaker(bool again = false);   // the speaker by the volume (struck through at 0); again: a new look
    void send(const QString &line);
    QString songName(int index) const;

    QLabel *m_title, *m_status, *m_timeLabel, *m_lengthLabel;
    QSlider *m_position, *m_volume;
    QToolButton *m_prev, *m_toggle, *m_next, *m_shuffle, *m_autoplay, *m_folder;
    MusicLevels *m_levels;
    QLabel *m_speaker;
    bool m_speakerOff = false;           // (as it is drawn now)
    QProcess m_proc;
    QByteArray m_buffer;
    QList<QPointer<QProcess>> m_watched;
    QTimer m_resume;                     // the moment after a hold ends
    bool m_held = false;
    bool m_bar = false;                  // laid out as a bar (arrange)

    QStringList m_songs;                 // the order they play in
    int m_index = -1;                    // the one playing or paused (-1: none)
    QString m_songTitle;                 // its name in the file, when it has one
    double m_at = 0, m_length = 0;       // where it is, as hiraku-midi last said, and how long it is
    bool m_playing = false;              // the player's wish: playing (paused, when false with a song)
    bool m_ready = false;                // the program said which synth: commands go now
    QString m_pending;                   // ... else the play command waits for it
    QString m_synth;                     // what it said
    QString m_problem;                   // the last thing that went wrong, shown under the title
    QString m_note;                      // the volume set back at start (musicvolume.h): said until the slider moves
    QProcess m_fetch;                    // tools/musicfetch.py, getting the first songs
    QString m_fetchNote;                 // ... how far, shown under the title
    int m_fetchFound = 0, m_fetchDone = 0;
    int m_errors = 0;                    // songs in a row it couldn't play
};
