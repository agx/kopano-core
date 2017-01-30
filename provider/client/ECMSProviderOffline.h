/*
 * Copyright 2005 - 2016 Zarafa and its licensors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef ECMSPROVIDEROFFLINE_H
#define ECMSPROVIDEROFFLINE_H

#include <kopano/zcdefs.h>
#include <kopano/ECUnknown.h>
#include "ECMSProvider.h"

class ECMSProviderOffline _kc_final : public ECMSProvider {
protected:
	ECMSProviderOffline(ULONG ulFlags);

public:
	static  HRESULT Create(ULONG ulFlags, ECMSProviderOffline **lppMSProvider);

	virtual HRESULT QueryInterface(REFIID refiid, void **lppInterface);

private:
	class xMSProvider _kc_final : public IMSProvider {
		#include <kopano/xclsfrag/IUnknown.hpp>
		#include <kopano/xclsfrag/IMSProvider.hpp>
	} m_xMSProvider;
};

#endif //#ifndef ECMSPROVIDEROFFLINE_H