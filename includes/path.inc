<?php
// $Id: path.inc,v 1.47 2009-10-24 05:13:43 webchick Exp $

/**
 * @file
 * Functions to handle paths in Drupal, including path aliasing.
 *
 * These functions are not loaded for cached pages, but modules that need
 * to use them in hook_boot() or hook exit() can make them available, by
 * executing "drupal_bootstrap(DRUPAL_BOOTSTRAP_FULL);".
 */

/**
 * Initialize the $_GET['q'] variable to the proper normal path.
 */
function drupal_path_initialize() {
  if (!empty($_GET['q'])) {
    $_GET['q'] = drupal_get_normal_path(trim($_GET['q'], '/'));
  }
  else {
    $_GET['q'] = drupal_get_normal_path(variable_get('site_frontpage', 'node'));
  }
}

/**
 * Given an alias, return its Drupal system URL if one exists. Given a Drupal
 * system URL return one of its aliases if such a one exists. Otherwise,
 * return FALSE.
 *
 * @param $action
 *   One of the following values:
 *   - wipe: delete the alias cache.
 *   - alias: return an alias for a given Drupal system path (if one exists).
 *   - source: return the Drupal system URL for a path alias (if one exists).
 * @param $path
 *   The path to investigate for corresponding aliases or system URLs.
 * @param $path_language
 *   Optional language code to search the path with. Defaults to the page language.
 *   If there's no path defined for that language it will search paths without
 *   language.
 *
 * @return
 *   Either a Drupal system path, an aliased path, or FALSE if no path was
 *   found.
 */
function drupal_lookup_path($action, $path = '', $path_language = '') {
  global $language;
  $cache = &drupal_static(__FUNCTION__, array(
    'map' => array(),
    'no_source' => array(),
    'whitelist' => NULL,
    'system_paths' => array(),
    'no_aliases' => array(),
    'first_call' => TRUE,
  ));

  // Retrieve the path alias whitelist.
  if (!isset($cache['whitelist'])) {
    $cache['whitelist'] = variable_get('path_alias_whitelist', NULL);
    if (!isset($cache['whitelist'])) {
      $cache['whitelist'] = drupal_path_alias_whitelist_rebuild();
    }
  }

  $path_language = $path_language ? $path_language : $language->language;

  if ($action == 'wipe') {
    $cache = array();
    $cache['whitelist'] = drupal_path_alias_whitelist_rebuild();
  }
  elseif ($cache['whitelist'] && $path != '') {
    if ($action == 'alias') {
      // During the first call to drupal_lookup_path() per language, load the
      // expected system paths for the page from cache.
      if (!empty($cache['first_call'])) {
        $cache['first_call'] = FALSE;

        $cache['map'][$path_language] = array();
        // Load system paths from cache.
        $cid = current_path();
        if ($cached = cache_get($cid, 'cache_path')) {
          $cache['system_paths'] = $cached->data;
          // Now fetch the aliases corresponding to these system paths.
          // We order by ASC and overwrite array keys to ensure the correct
          // alias is used when there are multiple aliases per path.
          $cache['map'][$path_language] = db_query("SELECT source, alias FROM {url_alias} WHERE source IN (:system) AND language IN (:language, '') ORDER BY language ASC, pid ASC", array(
            ':system' => $cache['system_paths'],
            ':language' => $path_language
          ))->fetchAllKeyed();
          // Keep a record of paths with no alias to avoid querying twice.
          $cache['no_aliases'][$path_language] = array_flip(array_diff_key($cache['system_paths'], array_keys($cache['map'][$path_language])));
        }
      }
      // If the alias has already been loaded, return it.
      if (isset($cache['map'][$path_language][$path])) {
        return $cache['map'][$path_language][$path];
      }
      // Check the path whitelist, if the top_level part before the first /
      // is not in the list, then there is no need to do anything further,
      // it is not in the database.
      elseif (!isset($cache['whitelist'][strtok($path, '/')])) {
        return FALSE;
      }
      // For system paths which were not cached, query aliases individually.
      else if (!isset($cache['no_aliases'][$path_language][$path])) {
        // Get the most fitting result falling back with alias without language
        $alias = db_query("SELECT alias FROM {url_alias} WHERE source = :source AND language IN (:language, '') ORDER BY language DESC, pid DESC", array(
          ':source' => $path,
          ':language' => $path_language
        ))->fetchField();
        $cache['map'][$path_language][$path] = $alias;
        return $alias;
      }
    }
    // Check $no_source for this $path in case we've already determined that there
    // isn't a path that has this alias
    elseif ($action == 'source' && !isset($cache['no_source'][$path_language][$path])) {
      // Look for the value $path within the cached $map
      $source = '';
      if (!isset($cache['map'][$path_language]) || !($source = array_search($path, $cache['map'][$path_language]))) {
        // Get the most fitting result falling back with alias without language
        if ($source = db_query("SELECT source FROM {url_alias} WHERE alias = :alias AND language IN (:language, '') ORDER BY language DESC, pid DESC", array(
                     ':alias' => $path,
                     ':language' => $path_language))
            ->fetchField()) {
          $cache['map'][$path_language][$source] = $path;
        }
        else {
          // We can't record anything into $map because we do not have a valid
          // index and there is no need because we have not learned anything
          // about any Drupal path. Thus cache to $no_source.
          $cache['no_source'][$path_language][$path] = TRUE;
        }
      }
      return $source;
    }
  }

  return FALSE;
}

/**
 * Cache system paths for a page.
 *
 * Cache an array of the system paths available on each page. We assume
 * that aiases will be needed for the majority of these paths during
 * subsequent requests, and load them in a single query during
 * drupal_lookup_path().
 */
function drupal_cache_system_paths() {
  // Check if the system paths for this page were loaded from cache in this
  // request to avoid writing to cache on every request.
  $cache = &drupal_static('drupal_lookup_path', array());
  if (!$cache['system_paths']) {
    // Generate a cache ID (cid) specifically for this page.
    $cid = current_path();
    // The static $map array used by drupal_lookup_path() includes all
    // system paths for the page request.
    if ($paths = current($cache['map'])) {
      $data = array_keys($paths);
      $expire = REQUEST_TIME + (60 * 60 * 24);
      cache_set($cid, $data, 'cache_path', $expire);
    }
  }
}

/**
 * Given an internal Drupal path, return the alias set by the administrator.
 *
 * If no path is provided, the function will return the alias of the current
 * page.
 *
 * @param $path
 *   An internal Drupal path.
 * @param $path_language
 *   An optional language code to look up the path in.
 *
 * @return
 *   An aliased path if one was found, or the original path if no alias was
 *   found.
 */
function drupal_get_path_alias($path = NULL, $path_language = '') {
  // If no path is specified, use the current page's path.
  if ($path == NULL) {
    $path = $_GET['q'];
  }
  $result = $path;
  if ($alias = drupal_lookup_path('alias', $path, $path_language)) {
    $result = $alias;
  }
  return $result;
}

/**
 * Given a path alias, return the internal path it represents.
 *
 * @param $path
 *   A Drupal path alias.
 * @param $path_language
 *   An optional language code to look up the path in.
 *
 * @return
 *   The internal path represented by the alias, or the original alias if no
 *   internal path was found.
 */
function drupal_get_normal_path($path, $path_language = '') {
  $original_path = $path;

  // Lookup the path alias first.
  if ($source = drupal_lookup_path('source', $path, $path_language)) {
    $path = $source;
  }

  // Allow other modules to alter the inbound URL. We cannot use drupal_alter()
  // here because we need to run hook_url_inbound_alter() in the reverse order
  // of hook_url_outbound_alter().
  foreach (array_reverse(module_implements('url_inbound_alter')) as $module) {
    $function = $module . '_url_inbound_alter';
    $function($path, $original_path, $path_language);
  }

  return $path;
}

/**
 * Return a component of the current Drupal path.
 *
 * When viewing a page at the path "admin/structure/types", for example, arg(0)
 * returns "admin", arg(1) returns "content", and arg(2) returns "types".
 *
 * Avoid use of this function where possible, as resulting code is hard to read.
 * In menu callback functions, attempt to use named arguments. See the explanation
 * in menu.inc for how to construct callbacks that take arguments. When attempting
 * to use this function to load an element from the current path, e.g. loading the
 * node on a node page, please use menu_get_object() instead.
 *
 * @param $index
 *   The index of the component, where each component is separated by a '/'
 *   (forward-slash), and where the first component has an index of 0 (zero).
 * @param $path
 *   A path to break into components. Defaults to the path of the current page.
 *
 * @return
 *   The component specified by $index, or NULL if the specified component was
 *   not found.
 */
function arg($index = NULL, $path = NULL) {
  $arguments = &drupal_static(__FUNCTION__);

  if (!isset($path)) {
    $path = $_GET['q'];
  }
  if (!isset($arguments[$path])) {
    $arguments[$path] = explode('/', $path);
  }
  if (!isset($index)) {
    return $arguments[$path];
  }
  if (isset($arguments[$path][$index])) {
    return $arguments[$path][$index];
  }
}

/**
 * Get the title of the current page, for display on the page and in the title bar.
 *
 * @return
 *   The current page's title.
 */
function drupal_get_title() {
  $title = drupal_set_title();

  // During a bootstrap, menu.inc is not included and thus we cannot provide a title.
  if (!isset($title) && function_exists('menu_get_active_title')) {
    $title = check_plain(menu_get_active_title());
  }

  return $title;
}

/**
 * Set the title of the current page, for display on the page and in the title bar.
 *
 * @param $title
 *   Optional string value to assign to the page title; or if set to NULL
 *   (default), leaves the current title unchanged.
 * @param $output
 *   Optional flag - normally should be left as CHECK_PLAIN. Only set to
 *   PASS_THROUGH if you have already removed any possibly dangerous code
 *   from $title using a function like check_plain() or filter_xss(). With this
 *   flag the string will be passed through unchanged.
 *
 * @return
 *   The updated title of the current page.
 */
function drupal_set_title($title = NULL, $output = CHECK_PLAIN) {
  $stored_title = &drupal_static(__FUNCTION__);

  if (isset($title)) {
    $stored_title = ($output == PASS_THROUGH) ? $title : check_plain($title);
  }

  return $stored_title;
}

/**
 * Check if the current page is the front page.
 *
 * @return
 *   Boolean value: TRUE if the current page is the front page; FALSE if otherwise.
 */
function drupal_is_front_page() {
  $is_front_page = &drupal_static(__FUNCTION__);

  if (!isset($is_front_page)) {
    // As drupal_path_initialize updates $_GET['q'] with the 'site_frontpage' path,
    // we can check it against the 'site_frontpage' variable.
    $is_front_page = ($_GET['q'] == drupal_get_normal_path(variable_get('site_frontpage', 'node')));
  }

  return $is_front_page;
}

/**
 * Check if a path matches any pattern in a set of patterns.
 *
 * @param $path
 *   The path to match.
 * @param $patterns
 *   String containing a set of patterns separated by \n, \r or \r\n.
 *
 * @return
 *   Boolean value: TRUE if the path matches a pattern, FALSE otherwise.
 */
function drupal_match_path($path, $patterns) {
  $regexps = &drupal_static(__FUNCTION__);

  if (!isset($regexps[$patterns])) {
    $regexps[$patterns] = '/^(' . preg_replace(array('/(\r\n?|\n)/', '/\\\\\*/', '/(^|\|)\\\\<front\\\\>($|\|)/'), array('|', '.*', '\1' . preg_quote(variable_get('site_frontpage', 'node'), '/') . '\2'), preg_quote($patterns, '/')) . ')$/';
  }
  return (bool)preg_match($regexps[$patterns], $path);
}

/**
 * Return the current URL path of the page being viewed.
 *
 * Examples:
 * - http://example.com/node/306 returns "node/306".
 * - http://example.com/drupalfolder/node/306 returns "node/306" while
 *   base_path() returns "/drupalfolder/".
 * - http://example.com/path/alias (which is a path alias for node/306) returns
 *   "node/306" as opposed to the path alias.
 *
 * This function is not available in hook_boot() so use $_GET['q'] instead.
 * However, be careful when doing that because in the case of Example #3
 * $_GET['q'] will contain "path/alias". If "node/306" is needed, calling
 * drupal_bootstrap(DRUPAL_BOOTSTRAP_FULL) makes this function available.
 *
 * @return
 *   The current Drupal URL path.
 */
function current_path() {
  return $_GET['q'];
}

/**
 * Rebuild the path alias white list.
 *
 * @return
 *   An array containing a white list of path aliases.
 */
function drupal_path_alias_whitelist_rebuild() {
  // For each alias in the database, get the top level component of the system
  // path it corresponds to. This is the portion of the path before the first /
  // if present, otherwise the whole path itself.
  $whitelist = array();
  $result = db_query("SELECT SUBSTRING_INDEX(source, '/', 1) AS path FROM {url_alias} GROUP BY path");
  foreach ($result as $row) {
    $whitelist[$row->path] = TRUE;
  }
  variable_set('path_alias_whitelist', $whitelist);
  return $whitelist;
}

/**
 * Fetch a specific URL alias from the database.
 *
 * @param $conditions
 *   A string representing the source, a number representing the pid, or an
 *   array of query conditions.
 *
 * @return
 *   FALSE if no alias was found or an associative array containing the
 *   following keys:
 *   - source: The internal system path.
 *   - alias: The URL alias.
 *   - pid: Unique path alias identifier.
 *   - language: The language of the alias.
 */
function path_load($conditions) {
  if (is_numeric($conditions)) {
    $conditions = array('pid' => $conditions);
  }
  elseif (is_string($conditions)) {
    $conditions = array('source' => $conditions);
  }
  elseif (!is_array($conditions)) {
    return FALSE;
  }
  $select = db_select('url_alias');
  foreach ($conditions as $field => $value) {
    $select->condition($field, $value);
  }
  return $select
    ->fields('url_alias')
    ->execute()
    ->fetchAssoc();
}

/**
 * Save a path alias to the database.
 *
 * @param $path
 *   An associative array containing the following keys:
 *   - source: The internal system path.
 *   - alias: The URL alias.
 *   - pid: (optional) Unique path alias identifier.
 *   - language: (optional) The language of the alias.
 */
function path_save(&$path) {
  $path += array('pid' => NULL, 'language' => '');

  // Insert or update the alias.
  $status = drupal_write_record('url_alias', $path, (!empty($path['pid']) ? 'pid' : NULL));

  // Verify that a record was written.
  if (isset($status) && $status) {
    if ($status === SAVED_NEW) {
      module_invoke_all('path_insert', $path);
    }
    else {
      module_invoke_all('path_update', $path);
    }
    drupal_clear_path_cache();
  }
}

/**
 * Delete a URL alias.
 *
 * @param $criteria
 *   A number representing the pid or an array of criteria.
 */
function path_delete($criteria) {
  if (!is_array($criteria)) {
    $criteria = array('pid' => $criteria);
  }
  $path = path_load($criteria);
  $query = db_delete('url_alias');
  foreach ($criteria as $field => $value) {
    $query->condition($field, $value);
  }
  $query->execute();
  module_invoke_all('path_delete', $path);
  drupal_clear_path_cache();
}

