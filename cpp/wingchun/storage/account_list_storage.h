//
// Created by qlu on 2019/3/8.
//

#ifndef KUNGFU_ACCOUNT_LIST_STORAGE_H
#define KUNGFU_ACCOUNT_LIST_STORAGE_H

#include <string>
#include <vector>
#include <SQLiteCpp/SQLiteCpp.h>
#include "oms_struct.h"

namespace kungfu
{
    namespace storage
    {
        class AccountListStorage
        {
        public:
            AccountListStorage(const std::string& file_name);
            ~AccountListStorage();
            void create_table_if_not_exist();
            void add_account(const std::string& client_id, const std::string& account_id, const std::string& source_id);
            std::vector<SubPortfolioInfo> get_accounts();
        private:
            SQLite::Database db_;
        };

    }
}

#endif //KUNGFU_ACCOUNT_LIST_STORAGE_H
