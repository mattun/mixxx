#pragma once
#include <QDomNode>
#include <QImage>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "track/cue.h"
#include "util/memory.h"
#include "waveform/waveformmarklabel.h"

class SkinContext;
class WaveformSignalColors;

class WOverview;

class WaveformMark {
  public:
    WaveformMark(
            const QString& group,
            const QDomNode& node,
            const SkinContext& context,
            const WaveformSignalColors& signalColors,
            int hotCue = Cue::kNoHotCue);

    // Disable copying
    WaveformMark(const WaveformMark&) = delete;
    WaveformMark& operator=(const WaveformMark&) = delete;

    int getHotCue() const { return m_iHotCue; };

    //The m_pPointCos related function
    bool isValid() const { return m_pPointCos && m_pPointCos->valid(); }

    template <typename Receiver, typename Slot>
    void connectSamplePositionChanged(Receiver receiver, Slot slot) const {
        m_pPointCos->connectValueChanged(receiver, slot, Qt::AutoConnection);
    };
    double getSamplePosition() const { return m_pPointCos->get(); }
    QString getItem() const { return m_pPointCos->getKey().item; }

    // The m_pVisibleCos related function
    bool hasVisible() const { return m_pVisibleCos && m_pVisibleCos->valid(); }
    bool isVisible() const {
        if (!hasVisible()) {
            return true;
        }
        return m_pVisibleCos->toBool();
    }

    template <typename Receiver, typename Slot>
    void connectVisibleChanged(Receiver receiver, Slot slot) const {
        m_pVisibleCos->connectValueChanged(receiver, slot, Qt::AutoConnection);
    }

    // Sets the appropriate mark colors based on the base color
    void setBaseColor(QColor baseColor, int dimBrightThreshold);
    QColor fillColor() const {
        return m_fillColor;
    }
    QColor borderColor() const {
        return m_borderColor;
    }
    QColor labelColor() const {
        return m_labelColor;
    }

    // Check if a point (in image coordinates) lies on drawn image.
    bool contains(QPoint point, Qt::Orientation orientation) const;

    QColor m_textColor;
    QString m_text;
    Qt::Alignment m_align;
    QString m_pixmapPath;

    float m_linePosition;

    WaveformMarkLabel m_label;

  private:
    std::unique_ptr<ControlProxy> m_pPointCos;
    std::unique_ptr<ControlProxy> m_pVisibleCos;
    int m_iHotCue;
    QImage m_image;

    QColor m_fillColor;
    QColor m_borderColor;
    QColor m_labelColor;

    friend class WaveformRenderMark;
};

typedef QSharedPointer<WaveformMark> WaveformMarkPointer;

// This class provides an immutable sortkey for the WaveformMark using sample
// position and hotcue number. IMPORTANT: The Mark's position may be changed after
// a key's creation, and those updates will not be reflected in these sortkeys.
// Currently they are used to render marks on the Overview, a situation where
// temporarily incorrect sort order is acceptable.
class WaveformMarkSortKey {
  public:
    WaveformMarkSortKey(double samplePosition, int hotcue)
            : m_samplePosition(samplePosition),
              m_hotcue(hotcue) {
    }

    bool operator<(const WaveformMarkSortKey& other) const {
        if (m_samplePosition == other.m_samplePosition) {
            // Sort WaveformMarks without hotcues before those with hotcues;
            // if both have hotcues, sort numerically by hotcue number.
            if (m_hotcue == Cue::kNoHotCue && other.m_hotcue != Cue::kNoHotCue) {
                return true;
            } else if (m_hotcue != Cue::kNoHotCue && other.m_hotcue == Cue::kNoHotCue) {
                return false;
            } else {
                return m_hotcue < other.m_hotcue;
            }
        }
        return m_samplePosition < other.m_samplePosition;
    }

  private:
    double m_samplePosition;
    int m_hotcue;
};
