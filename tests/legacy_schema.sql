-- Synthetic, disabled accounts only. Never import into the production database.
CREATE TABLE organization (
  organization_id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  name VARCHAR(50) NOT NULL UNIQUE,
  default_domain VARCHAR(50) NOT NULL,
  inbound_mailserver VARCHAR(128) NOT NULL,
  outbound_mailserver VARCHAR(128) NOT NULL,
  hosted_here ENUM('yes','no') NOT NULL DEFAULT 'yes',
  filterctl ENUM('yes','no') NOT NULL DEFAULT 'yes',
  spammgt ENUM('yes','no') NOT NULL DEFAULT 'no',
  spamfld VARCHAR(64) NOT NULL DEFAULT 'spam',
  spamdays INT NOT NULL DEFAULT 30,
  spamtrash VARCHAR(64) NOT NULL DEFAULT 'Trash'
) ENGINE=InnoDB DEFAULT CHARSET=latin1;
CREATE TABLE passwd (
  passwd_id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  id CHAR(128) NOT NULL UNIQUE,
  crypt CHAR(128) NOT NULL DEFAULT '',
  clear CHAR(128) NOT NULL DEFAULT '',
  name CHAR(128) NOT NULL DEFAULT '',
  uid INT UNSIGNED NOT NULL,
  gid INT UNSIGNED NOT NULL,
  home CHAR(255) NOT NULL DEFAULT '',
  maildir CHAR(255) NOT NULL DEFAULT '',
  quota CHAR(255) NOT NULL DEFAULT '',
  organization_id INT NOT NULL,
  active ENUM('yes','no') NOT NULL DEFAULT 'yes',
  old_popname VARCHAR(50) NOT NULL DEFAULT '',
  filterctl ENUM('yes','no') NULL,
  spammgt ENUM('yes','no') NULL,
  spamfld VARCHAR(64) NULL,
  spamdays INT NULL,
  spamtrash VARCHAR(64) NULL,
  oomsg MEDIUMTEXT NULL,
  keep_copy ENUM('yes','no') NOT NULL DEFAULT 'no',
  INDEX(organization_id)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;
CREATE TABLE alias (
  alias_id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  mailaddr CHAR(128) NOT NULL UNIQUE,
  passwd_id INT NOT NULL,
  organization_id INT NOT NULL,
  INDEX(passwd_id), INDEX(organization_id)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;
CREATE TABLE forward (
  forward_id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  passwd_id INT NOT NULL,
  destination VARCHAR(128) NOT NULL,
  INDEX(passwd_id)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;
