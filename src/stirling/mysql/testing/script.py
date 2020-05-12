import os
import mysql.connector

# MySQL Python API: https://dev.mysql.com/doc/connector-python/en/connector-python-reference.html

config = {
    "user": "root",
    "host": "0.0.0.0",
    "port": "33060",
    "password": "",
    "ssl_disabled": "True"
}

create_db_command = "CREATE DATABASE employee DEFAULT CHARACTER SET 'utf8'"
drop_db_command = "DROP DATABASE employee"
create_table_command = (
    "CREATE TABLE `employees` ("
    "  `emp_no` int(11) NOT NULL AUTO_INCREMENT,"
    "  `first_name` varchar(14) NOT NULL,"
    "  `last_name` varchar(16) NOT NULL,"
    "  `gender` enum('M','F') NOT NULL,"
    "  PRIMARY KEY (`emp_no`)"
    ") ENGINE=InnoDB"
)

insert_command = (
    "INSERT INTO employees "
    "(first_name, last_name, gender) "
    "VALUES (%s, %s, %s)"
)

select_command = "SELECT * FROM employees"

employee_data = [
    ('First0', 'Last0', 'M'),
    ('First1', 'Last1', 'F'),
    ('First2', 'Last2', 'M'),
]


def main():
    print(os.getpid())
    cnx = mysql.connector.connect(**config)

    # kStatistics 0x09
    # TODO(chengruizhe): MySQL parser currently doesn't support kStatistics.
    #  Turn on when it's supported.
    # cnx.cmd_statistics()

    # kQuery 0x03
    cnx.cmd_query(create_db_command)

    # kInitDB 0x02
    cnx.cmd_init_db("employee")
    cnx.cmd_query(create_table_command)

    # Get cursor for StmtPrepare & StmtExecute
    cursor = cnx.cursor(prepared=True)
    # kStmtPrepare 0x16
    # kStmtExecute 0x17
    # kStmtReset 0x1a
    cursor.executemany(insert_command, employee_data)

    cnx.commit()

    # kStmtClose 0x19
    cursor.execute(select_command)
    rows = cursor.fetchall()
    print(rows)

    cursor.close()

    cnx.disconnect()
    cnx.reconnect()

    # kQuit 0x01
    cnx.close()


# Not implemented commands
# kDebug
# kPing
# kResetConnection

# Deprecated commands
# kShutdown
# TODO(chengruizhe): Create a table to record if each command is covered,
#   deprecated, or not covered.


if __name__ == "__main__":
    main()
