# This file is part of the Flowee project
# Copyright (C) 2018 Tom Zander <tomz@freedommail.ch>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

include_directories(${FL_LIBWALLET_INCLUDES})

add_library(flowee_wallet STATIC
    crypter.cpp
    db.cpp
    rpcdump.cpp
    rpcwallet.cpp
    wallet.cpp
    walletdb.cpp
    wallet_ismine.cpp
)

target_link_libraries(flowee_wallet flowee_server)
