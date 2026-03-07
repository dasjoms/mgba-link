/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include "netplay/SessionTypes.h"

namespace QGBA {
namespace Netplay {

struct CodecError {
	int code = 0;
	QString message;
	QVariantMap details;
	NetplayErrorCategory category = NetplayErrorCategory::MalformedMessage;

	explicit operator bool() const {
		return code != 0;
	}
};

struct DecodedMessage {
	QString kind;
	QVariantMap payload;
	CodecError error;

	bool isValid() const {
		return !static_cast<bool>(error);
	}
};

QByteArray encodeFrame(const QVariantMap& payload, CodecError* error = nullptr);
DecodedMessage decodeFrame(const QByteArray& payload);

} // namespace Netplay
} // namespace QGBA
