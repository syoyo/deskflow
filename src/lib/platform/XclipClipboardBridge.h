/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "deskflow/IClipboard.h"
#include <QByteArray>
#include <QString>
#include <mutex>

namespace deskflow {
class EiClipboard;

// Opt-in Xwayland clipboard bridge for compositors without InputCapture v2.
// Adapted from the xclip approach in coekfung/deskflow's dev-xclip branch.
// Only CLIPBOARD is used; PRIMARY (mouse selection) is intentionally separate.
class XclipClipboardBridge
{
public:
  explicit XclipClipboardBridge(QString executable);
  bool refresh(EiClipboard *cache, qint64 maxBytes);
  bool publish(const IClipboard *source, EiClipboard *cache, qint64 maxBytes);
  bool copyTo(IClipboard *destination, EiClipboard *cache);

private:
  bool read(const QString &target, QByteArray &data, qint64 maxBytes) const;
  bool write(const QString &target, const QByteArray &data) const;
  static QByteArray fingerprint(IClipboard::Format format, const QByteArray &data);
  QString m_executable;
  QByteArray m_lastFingerprint;
  std::mutex m_mutex;
};
} // namespace deskflow
